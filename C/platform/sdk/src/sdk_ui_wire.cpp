// SAO Auto — SDK UI wire.
//
// Forwards `SaoSdkContext::ui->*` calls into the platform UI module:
//   * register_ui_panel      → sao_ui_panel_register (SaoPanelDescriptor)
//   * unregister_ui_panel    → sao_ui_panel_unregister
//   * panel_add_widget       → widget kit create + sao_ui_panel_update_body
//   * panel_update_widget    → widget-kit-specific update
//   * panel_remove_widget    → sao_ui_panel_update_body(REMOVE_NODE)
//   * request_redraw         → sao_ui_panel_bring_to_front + counter bump
//   * register_render_hook_clock — SDK-local dispatcher (fired via
//     sao_sdk_internal::fire_render_hook_test in tests; the real
//     compositor will call the same entry point in production).
//
// The legacy JSON-spec register_panel/set_panel_spec/set_overlay paths
// exist for scripting-language plugins that emit `ui_spec` blobs; they
// share the same PanelEntry storage.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

// Include the specific widget headers directly.  The umbrella
// (sao/ui/widget_kit.h) re-declares an extended kind enum whose
// values collide with the legacy d2d_widgets.h enum — pulling it in
// alongside widget_input/text/data would double-define several
// enumerators.
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_text.h"

extern "C" void SAO_UI_CALL sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);

namespace sao_sdk_internal {
namespace {

std::atomic_bool g_fail_next_panel_state_insertion{false};
std::atomic<sao_sdk_status_t> g_fail_next_panel_unregister_status{SAO_SDK_OK};

bool valid_widget_kind(int32_t kind) noexcept {
    switch (kind) {
    case SAO_SDK_UI_WIDGET_LABEL:
    case SAO_SDK_UI_WIDGET_BUTTON:
    case SAO_SDK_UI_WIDGET_PROGRESS_BAR:
    case SAO_SDK_UI_WIDGET_TABLE:
    case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:
    case SAO_SDK_UI_WIDGET_STATUS_BADGE:
    case SAO_SDK_UI_WIDGET_TEXT_FIELD:
    case SAO_SDK_UI_WIDGET_CHECKBOX:
    case SAO_SDK_UI_WIDGET_DIVIDER:
    case SAO_SDK_UI_WIDGET_ICON:
        return true;
    default:
        return false;
    }
}

sao_sdk_status_t unregister_native_panel(sao_ui_panel_handle_t panel) noexcept {
    const auto injected = g_fail_next_panel_unregister_status.exchange(SAO_SDK_OK);
    if (injected != SAO_SDK_OK)
        return injected;
    return invoke_callback_barrier(
        [&] { return static_cast<sao_sdk_status_t>(sao_ui_panel_unregister(panel)); });
}

// ─── Widget instantiation helper ─────────────────────────────────────
//
// Translates SaoSdkWidgetSpec into the widget-kit call for the target
// kind.  Returns a widget handle (may be null on unimplemented kinds).
void append_json_string(std::string& json, const char* value) {
    json.push_back('"');
    if (value != nullptr) {
        for (const auto* cursor = reinterpret_cast<const unsigned char*>(value); *cursor != 0;
             ++cursor) {
            switch (*cursor) {
            case '"':
                json.append("\\\"");
                break;
            case '\\':
                json.append("\\\\");
                break;
            case '\b':
                json.append("\\b");
                break;
            case '\f':
                json.append("\\f");
                break;
            case '\n':
                json.append("\\n");
                break;
            case '\r':
                json.append("\\r");
                break;
            case '\t':
                json.append("\\t");
                break;
            default:
                if (*cursor < 0x20U) {
                    char escaped[7]{};
                    (void)std::snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                    json.append(escaped);
                } else {
                    json.push_back(static_cast<char>(*cursor));
                }
                break;
            }
        }
    }
    json.push_back('"');
}

std::string generic_widget_props(const SaoSdkWidgetSpec& spec) {
    std::string props{"{\"active\":true"};
    if (spec.text_utf8 != nullptr) {
        props.append(",\"text\":");
        append_json_string(props, spec.text_utf8);
    }
    if (spec.kind == SAO_SDK_UI_WIDGET_PROGRESS_BAR) {
        const float maximum = spec.max_value > 0.0F ? spec.max_value : 1.0F;
        const float ratio = std::clamp(spec.value / maximum, 0.0F, 1.0F);
        char value[48]{};
        (void)std::snprintf(value, sizeof(value), ",\"value\":%.9g", ratio);
        props.append(value);
    }
    if (spec.props_json_utf8 != nullptr && spec.props_len >= 2 && spec.props_json_utf8[0] == '{' &&
        spec.props_json_utf8[spec.props_len - 1] == '}') {
        const auto* extra = reinterpret_cast<const char*>(spec.props_json_utf8 + 1);
        const size_t extra_size = spec.props_len - 2;
        if (extra_size != 0) {
            props.push_back(',');
            props.append(extra, extra_size);
        }
    }
    props.push_back('}');
    return props;
}

sao_ui_widget_handle_t create_widget_for_kind(const SaoSdkWidgetSpec& spec) {
    if (!valid_widget_kind(spec.kind))
        return nullptr;
    sao_ui_widget_handle_t handle = nullptr;
    int32_t native_kind = SAO_UI_WIDGET_ROUNDED_PANEL;
    switch (spec.kind) {
    case SAO_SDK_UI_WIDGET_LABEL:
        native_kind = SAO_UI_WIDGET_TEXT;
        break;
    case SAO_SDK_UI_WIDGET_BUTTON:
        native_kind = SAO_UI_WIDGET_ACTION_BUTTON;
        break;
    case SAO_SDK_UI_WIDGET_PROGRESS_BAR:
        native_kind = SAO_UI_WIDGET_BAR;
        break;
    case SAO_SDK_UI_WIDGET_TABLE:
        native_kind = SAO_UI_WIDGET_TABLE;
        break;
    case SAO_SDK_UI_WIDGET_STATUS_BADGE:
        native_kind = SAO_UI_WIDGET_STATUS_BADGE;
        break;
    case SAO_SDK_UI_WIDGET_TEXT_FIELD:
        native_kind = SAO_UI_WIDGET_INPUT;
        break;
    case SAO_SDK_UI_WIDGET_CHECKBOX:
        native_kind = SAO_UI_WIDGET_CHECKBOX;
        break;
    case SAO_SDK_UI_WIDGET_DIVIDER:
        native_kind = SAO_UI_WIDGET_DIVIDER;
        break;
    case SAO_SDK_UI_WIDGET_ICON:
        native_kind = SAO_UI_WIDGET_ICON;
        break;
    case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:
        break;
    default:
        return nullptr;
    }
    if (sao_ui_widget_create(native_kind, nullptr, &handle) != SAO_STATUS_OK)
        return nullptr;
    const auto props = generic_widget_props(spec);
    if (sao_ui_widget_apply_props(handle, reinterpret_cast<const uint8_t*>(props.data()),
                                  props.size()) != SAO_STATUS_OK) {
        sao_ui_widget_destroy(handle);
        return nullptr;
    }
    return handle;
}

void update_widget_for_kind(sao_ui_widget_handle_t widget, const SaoSdkWidgetSpec& spec) {
    if (widget == nullptr)
        return;
    const auto props = generic_widget_props(spec);
    (void)sao_ui_widget_apply_props(widget, reinterpret_cast<const uint8_t*>(props.data()),
                                    props.size());
}

void clear_legacy_canvases(PanelEntry& panel) {
    for (const auto canvas : panel.canvases) {
        sao_ui_script_canvas_destroy(canvas);
    }
    panel.canvases.clear();
    for (const auto placeholder : panel.canvas_placeholders) {
        sao_ui_widget_destroy(placeholder);
    }
    panel.canvas_placeholders.clear();
}

void destroy_panel_resources(PanelEntry& panel) {
    clear_legacy_canvases(panel);
    for (const auto& widget : panel.widgets)
        destroy_widget_for_kind(widget.kind, widget.ui_widget);
    panel.widgets.clear();
}

sao_sdk_status_t materialize_legacy_canvas(PanelEntry& panel, const uint8_t* spec_json_utf8,
                                           size_t spec_len) {
    if (spec_json_utf8 == nullptr || spec_len == 0)
        return SAO_SDK_OK;
    const std::string spec(reinterpret_cast<const char*>(spec_json_utf8), spec_len);
    if (spec.find("\"canvas\"") == std::string::npos)
        return SAO_SDK_OK;

    SaoUiScriptCanvasSpec canvas_spec{};
    canvas_spec.width_px = 640;
    canvas_spec.height_px = 360;
    canvas_spec.antialias = true;
    canvas_spec.retain_ops_between_frames = true;
    canvas_spec.max_ops_per_frame = 4000;
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t canvas = nullptr;
    const sao_status_t canvas_rc =
        sao_ui_script_canvas_create(nullptr, &canvas_spec, &widget, &canvas);
    if (canvas_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(canvas_rc);

    sao_ui_widget_handle_t placeholder = nullptr;
    if (sao_ui_widget_create(SAO_UI_WIDGET_ROUNDED_PANEL, nullptr, &placeholder) != SAO_STATUS_OK) {
        sao_ui_script_canvas_destroy(canvas);
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    static constexpr char kCanvasProps[] = R"({"active":true})";
    if (sao_ui_widget_apply_props(placeholder, reinterpret_cast<const uint8_t*>(kCanvasProps),
                                  sizeof(kCanvasProps) - 1) != SAO_STATUS_OK) {
        sao_ui_widget_destroy(placeholder);
        sao_ui_script_canvas_destroy(canvas);
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }

    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = canvas_spec.width_px;
    layout.fixed_height_px = canvas_spec.height_px;
    layout.hit_testable = true;
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_ADD_WIDGET;
    mutation.widget = placeholder;
    mutation.spec = &layout;
    const sao_status_t panel_rc = sao_ui_panel_update_body(panel.ui_body, &mutation, 1);
    if (panel_rc != SAO_STATUS_OK) {
        sao_ui_widget_destroy(placeholder);
        sao_ui_script_canvas_destroy(canvas);
        return static_cast<sao_sdk_status_t>(panel_rc);
    }
    panel.canvases.push_back(canvas);
    panel.canvas_placeholders.push_back(placeholder);
    return SAO_SDK_OK;
}

// ─── UI table vtable functions ───────────────────────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(void* ctx_impl,
                                                   const SaoSdkPanelDescriptor* descriptor,
                                                   sao_sdk_ui_panel_t* out_panel);

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(void* ctx_impl, sao_sdk_ui_panel_t panel);

sao_sdk_status_t SAO_SDK_CALL ui_register_panel(void* ctx_impl, const char* panel_id_utf8,
                                                const char* title_utf8,
                                                const uint8_t* initial_spec_json_utf8,
                                                size_t spec_len,
                                                sao_sdk_panel_action_callback_t action_cb,
                                                void* action_user_data,
                                                sao_sdk_ui_panel_t* out_panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    if (initial_spec_json_utf8 == nullptr && spec_len != 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = panel_id_utf8;
    descriptor.title_utf8 = title_utf8;
    descriptor.default_width_px = 480;
    descriptor.default_height_px = 320;
    descriptor.min_width_px = 160;
    descriptor.min_height_px = 100;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.remember_geometry = true;
    descriptor.initial_opacity = 1.0f;

    const sao_sdk_status_t register_rc = ui_register_ui_panel(ctx_impl, &descriptor, out_panel);
    if (register_rc != SAO_SDK_OK)
        return register_rc;

    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(*out_panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_HANDLE_INVALID;
        it->second.legacy_action_cb = action_cb;
        it->second.legacy_action_user_data = action_user_data;
        body = it->second.ui_body;
    }
    if (initial_spec_json_utf8 != nullptr) {
        const sao_status_t spec_rc =
            sao_ui_panel_body_set_spec(body, initial_spec_json_utf8, spec_len);
        if (spec_rc != SAO_STATUS_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return static_cast<sao_sdk_status_t>(spec_rc);
        }
        sao_sdk_status_t canvas_rc = SAO_SDK_ERR_HANDLE_INVALID;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto it = state->panels.find(*out_panel);
            if (it != state->panels.end()) {
                canvas_rc = materialize_legacy_canvas(it->second, initial_spec_json_utf8, spec_len);
            }
        }
        if (canvas_rc != SAO_SDK_OK) {
            (void)ui_unregister_ui_panel(ctx_impl, *out_panel);
            *out_panel = nullptr;
            return canvas_rc;
        }
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering)
            return SAO_SDK_ERR_BUSY;
        body = it->second.ui_body;
    }
    const sao_status_t spec_rc = sao_ui_panel_body_set_spec(body, spec_json_utf8, spec_len);
    if (spec_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(spec_rc);
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    if (it->second.unregistering)
        return SAO_SDK_ERR_BUSY;
    clear_legacy_canvases(it->second);
    return materialize_legacy_canvas(it->second, spec_json_utf8, spec_len);
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay(void* ctx_impl, const char* surface_id_utf8,
                                             const uint8_t* spec_json_utf8, size_t spec_len) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' ||
        (spec_json_utf8 == nullptr && spec_len != 0)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    sao_sdk_overlay_token_t previous = 0;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->overlays.find(surface_id_utf8);
        if (it != state->overlays.end()) {
            previous = reinterpret_cast<uint64_t>(it->second);
        }
    }
    if (previous != 0) {
        const auto clear_status = provider_overlay_clear(state, previous);
        if (clear_status != SAO_SDK_OK)
            return clear_status;
    }
    SaoSdkOverlaySpec spec{};
    spec.surface_id_utf8 = surface_id_utf8;
    spec.spec_json_utf8 = spec_json_utf8;
    spec.spec_len = spec_len;
    sao_sdk_overlay_token_t overlay = 0;
    return provider_overlay_set(state, &spec, &overlay);
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_legacy(void* ctx_impl,
                                                             const char* surface_id_utf8,
                                                             float priority, void* hook_fn,
                                                             void* hook_user_data,
                                                             sao_sdk_hook_token_t* out_token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_token != nullptr)
        *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' || hook_fn == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    (void)priority;
    (void)hook_user_data;
    return SAO_SDK_ERR_UNSUPPORTED;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy(void* ctx_impl,
                                                               sao_sdk_hook_token_t token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return provider_render_unregister(cast_ctx(ctx_impl), token);
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw(void* ctx_impl, const char* surface_id_utf8) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    const char* surface = surface_id_utf8 == nullptr ? SAO_ENGINE_ALL_SURFACES : surface_id_utf8;
    const auto status = provider_request_redraw(state, surface);
    if (status != SAO_SDK_OK)
        return status;
    std::lock_guard<std::mutex> lk(state->mu);
    for (auto& [handle, panel] : state->panels) {
        (void)handle;
        if (surface_id_utf8 == nullptr || panel.panel_id == surface_id_utf8) {
            ++panel.redraw_count;
        }
    }
    return SAO_SDK_OK;
}

// ─── Append-only typed panel and widget fields ─────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(void* ctx_impl,
                                                   const SaoSdkPanelDescriptor* descriptor,
                                                   sao_sdk_ui_panel_t* out_panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (descriptor == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (descriptor->panel_id_utf8 == nullptr || descriptor->panel_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    // Build a full SaoPanelDescriptor from the SDK subset.
    SaoPanelDescriptor full{};
    full.panel_id_utf8 = descriptor->panel_id_utf8;
    full.title_utf8 = descriptor->title_utf8;
    full.anchor = SAO_UI_PANEL_ANCHOR_ABSOLUTE;
    full.default_x_px = descriptor->default_x_px;
    full.default_y_px = descriptor->default_y_px;
    full.default_width_px = descriptor->default_width_px;
    full.default_height_px = descriptor->default_height_px;
    full.min_width_px = descriptor->min_width_px;
    full.min_height_px = descriptor->min_height_px;
    full.max_width_px = 0;
    full.max_height_px = 0;
    full.movable = descriptor->movable;
    full.resizable = descriptor->resizable;
    full.show_titlebar = descriptor->show_titlebar;
    full.show_close_button = descriptor->show_close_button;
    full.visible = descriptor->visible;
    full.remember_geometry = descriptor->remember_geometry;
    full.modal = descriptor->modal;
    full.overlay_style = descriptor->overlay_style;
    full.z_class = descriptor->z_class;
    full.z_within_class = descriptor->z_within_class;
    full.initial_opacity = descriptor->initial_opacity;

    std::list<PanelEntry>::iterator pending;
    try {
        PanelEntry entry;
        entry.panel_id = descriptor->panel_id_utf8;
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.push_back(std::move(entry));
        pending = std::prev(state->panel_cleanup_pending.end());
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }

    auto& rt = SharedRuntime::instance();
    sao_ui_panel_handle_t ui_panel = nullptr;
    sao_ui_panel_body_handle_t ui_body = nullptr;
    const auto register_status = invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return static_cast<sao_sdk_status_t>(
            sao_ui_panel_register(rt.compositor, &full, &ui_panel, &ui_body));
    });
    if (register_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.erase(pending);
        if (register_status == SAO_STATUS_ERR_ALREADY_EXISTS)
            return SAO_SDK_ERR_ALREADY_EXISTS;
        if (register_status == SAO_STATUS_ERR_INVALID_ARGUMENT)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return register_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        pending->sdk_handle = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
        pending->ui_panel = ui_panel;
        pending->ui_body = ui_body;
    }
    if (ui_panel == nullptr || ui_body == nullptr) {
        const auto rollback_status =
            ui_panel == nullptr ? SAO_SDK_OK : unregister_native_panel(ui_panel);
        if (rollback_status == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->panel_cleanup_pending.erase(pending);
        }
        return SAO_SDK_ERR_HANDLE_INVALID;
    }

    pause_context_api_test_point(ContextApiTestPoint::panel_registered);
    sao_sdk_status_t insertion_status = SAO_SDK_OK;
    try {
        if (g_fail_next_panel_state_insertion.exchange(false))
            throw std::bad_alloc{};
        std::lock_guard<std::mutex> lk(state->mu);
        const auto handle = pending->sdk_handle;
        if (state->panels.find(handle) != state->panels.end()) {
            insertion_status = SAO_SDK_ERR_ALREADY_EXISTS;
        } else {
            state->panels.emplace(handle, *pending);
            state->panel_cleanup_pending.erase(pending);
        }
    } catch (...) {
        insertion_status = SAO_SDK_ERR_INTERNAL;
    }
    if (insertion_status != SAO_SDK_OK) {
        const auto rollback_status = unregister_native_panel(ui_panel);
        if (rollback_status == SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->panel_cleanup_pending.erase(pending);
        }
        return rollback_status == SAO_SDK_OK ? insertion_status : rollback_status;
    }
    *out_panel = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(void* ctx_impl, sao_sdk_ui_panel_t panel) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    sao_ui_panel_handle_t native_panel = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->second.unregistering)
            return SAO_SDK_ERR_BUSY;
        it->second.unregistering = true;
        native_panel = it->second.ui_panel;
    }

    const auto unregister_status = unregister_native_panel(native_panel);
    if (unregister_status != SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = state->panels.find(panel);
        if (found != state->panels.end())
            found->second.unregistering = false;
        return unregister_status;
    }

    PanelEntry retired;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return SAO_SDK_ERR_NOT_FOUND;
        retired = std::move(it->second);
        for (auto overlay = state->overlays.begin(); overlay != state->overlays.end();) {
            if (overlay->second == panel) {
                overlay = state->overlays.erase(overlay);
            } else {
                ++overlay;
            }
        }
        state->panels.erase(it);
    }
    destroy_panel_resources(retired);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_add_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                  const SaoSdkWidgetSpec* widget_spec,
                                                  sao_sdk_ui_widget_t* out_widget) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_widget != nullptr)
        *out_widget = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr || widget_spec == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (!valid_widget_kind(widget_spec->kind))
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    // Create the underlying platform widget outside the lock — some
    // widget_kit paths take their own locks.
    sao_ui_widget_handle_t ui_widget = create_widget_for_kind(*widget_spec);
    if (ui_widget == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;

    pause_context_api_test_point(ContextApiTestPoint::panel_operation_unlocked);
    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) {
        destroy_widget_for_kind(widget_spec->kind, ui_widget);
        return SAO_SDK_ERR_NOT_FOUND;
    }
    PanelEntry& pe = it->second;
    if (pe.unregistering) {
        destroy_widget_for_kind(widget_spec->kind, ui_widget);
        return SAO_SDK_ERR_BUSY;
    }

    // Reject duplicate widget_id inside the same panel.
    if (widget_spec->widget_id_utf8 != nullptr && widget_spec->widget_id_utf8[0] != '\0') {
        for (const auto& w : pe.widgets) {
            if (w.widget_id == widget_spec->widget_id_utf8) {
                destroy_widget_for_kind(widget_spec->kind, ui_widget);
                return SAO_SDK_ERR_ALREADY_EXISTS;
            }
        }
    }

    // Batch mutation so the platform sees one dirty-rects snapshot.
    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = widget_spec->width_px;
    layout.fixed_height_px = widget_spec->height_px;
    layout.absolute_x_px = widget_spec->x_px;
    layout.absolute_y_px = widget_spec->y_px;
    layout.absolute_z = widget_spec->z_order;
    layout.hit_testable = true;

    sao_ui_layout_node_handle_t new_node = nullptr;
    SaoUiBodyMutation mut{};
    mut.kind = SAO_UI_BODY_ADD_WIDGET;
    mut.target = nullptr; // root of body
    mut.widget = ui_widget;
    mut.spec = &layout;
    mut.out_new_node = &new_node;
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK) {
        destroy_widget_for_kind(widget_spec->kind, ui_widget);
        return static_cast<sao_sdk_status_t>(update_rc);
    }
    update_widget_for_kind(ui_widget, *widget_spec);

    WidgetEntry we;
    we.sdk_handle =
        reinterpret_cast<sao_sdk_ui_widget_t>(reinterpret_cast<uintptr_t>(&pe) + pe.next_widget_id);
    we.ui_widget = ui_widget;
    pe.next_widget_id += 8;
    we.widget_id =
        (widget_spec->widget_id_utf8 == nullptr) ? std::string() : widget_spec->widget_id_utf8;
    we.kind = widget_spec->kind;
    we.layout_node = new_node;
    pe.widgets.push_back(std::move(we));

    if (out_widget != nullptr) {
        *out_widget = pe.widgets.back().sdk_handle;
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_update_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     sao_sdk_ui_widget_t widget,
                                                     const SaoSdkWidgetSpec* widget_spec) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;
    if (pe.unregistering)
        return SAO_SDK_ERR_BUSY;

    WidgetEntry* target = nullptr;
    for (auto& w : pe.widgets) {
        if (w.sdk_handle == widget) {
            target = &w;
            break;
        }
    }
    if (target == nullptr)
        return SAO_SDK_ERR_NOT_FOUND;

    // The layout-node handle produced by sao_ui_panel_update_body is a
    // synthetic value in the current slice — passing it as the update
    // target still logs the mutation in the panel body so callers see
    // one dirty-rects batch.
    SaoUiBodyMutation mut{};
    mut.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mut.target = target->layout_node;
    mut.widget = target->ui_widget;
    const auto props = generic_widget_props(*widget_spec);
    mut.props_json_utf8 = reinterpret_cast<const uint8_t*>(props.data());
    mut.props_len = props.size();
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(update_rc);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget(void* ctx_impl, sao_sdk_ui_panel_t panel,
                                                     sao_sdk_ui_widget_t widget) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end())
        return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;
    if (pe.unregistering)
        return SAO_SDK_ERR_BUSY;

    auto pred = [widget](const WidgetEntry& w) { return w.sdk_handle == widget; };
    auto rem = std::find_if(pe.widgets.begin(), pe.widgets.end(), pred);
    if (rem == pe.widgets.end())
        return SAO_SDK_ERR_NOT_FOUND;

    SaoUiBodyMutation mut{};
    mut.kind = SAO_UI_BODY_REMOVE_NODE;
    mut.target = rem->layout_node;
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(update_rc);
    destroy_widget_for_kind(rem->kind, rem->ui_widget);
    pe.widgets.erase(rem);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_clock(void* ctx_impl, int32_t hook_point,
                                                            sao_sdk_render_hook_callback_t callback,
                                                            void* user_data,
                                                            sao_sdk_hook_token_t* out_token) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_token != nullptr)
        *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (callback == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    return provider_render_register(state, hook_point, callback, user_data, out_token);
}

sao_sdk_status_t SAO_SDK_CALL ui_register_panel_boundary(
    void* ctx_impl, const char* panel_id_utf8, const char* title_utf8,
    const uint8_t* initial_spec_json_utf8, size_t spec_len,
    sao_sdk_panel_action_callback_t action_cb, void* action_user_data,
    sao_sdk_ui_panel_t* out_panel) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_panel(ctx_impl, panel_id_utf8, title_utf8, initial_spec_json_utf8,
                                 spec_len, action_cb, action_user_data, out_panel);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, const uint8_t* spec_json_utf8,
    size_t spec_len) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_set_panel_spec(ctx_impl, panel, spec_json_utf8, spec_len); });
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay_boundary(
    void* ctx_impl, const char* surface_id_utf8, const uint8_t* spec_json_utf8,
    size_t spec_len) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_set_overlay(ctx_impl, surface_id_utf8, spec_json_utf8, spec_len); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_legacy_boundary(
    void* ctx_impl, const char* surface_id_utf8, float priority, void* hook_fn,
    void* hook_user_data, sao_sdk_hook_token_t* out_token) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_render_hook_legacy(ctx_impl, surface_id_utf8, priority, hook_fn,
                                              hook_user_data, out_token);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy_boundary(
    void* ctx_impl, sao_sdk_hook_token_t token) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_unregister_render_hook_legacy(ctx_impl, token); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_clock_boundary(
    void* ctx_impl, int32_t hook_point, sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token) noexcept {
    return invoke_callback_barrier([&] {
        return ui_register_render_hook_clock(ctx_impl, hook_point, callback, user_data, out_token);
    });
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw_boundary(
    void* ctx_impl, const char* surface_id_utf8) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_request_redraw(ctx_impl, surface_id_utf8); });
}

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel_boundary(
    void* ctx_impl, const SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_register_ui_panel(ctx_impl, descriptor, out_panel); });
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_unregister_ui_panel(ctx_impl, panel); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_add_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, const SaoSdkWidgetSpec* widget_spec,
    sao_sdk_ui_widget_t* out_widget) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_add_widget(ctx_impl, panel, widget_spec, out_widget); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_update_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget,
    const SaoSdkWidgetSpec* widget_spec) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_update_widget(ctx_impl, panel, widget, widget_spec); });
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget_boundary(
    void* ctx_impl, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) noexcept {
    return invoke_callback_barrier(
        [&] { return ui_panel_remove_widget(ctx_impl, panel, widget); });
}

} // namespace

void destroy_widget_for_kind(int32_t kind, sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return;
    (void)kind;
    sao_ui_widget_destroy(widget);
}

sao_sdk_status_t cleanup_ui_panels(ContextState* state) {
    std::list<PanelEntry> pending;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        for (auto& [handle, panel] : state->panels) {
            (void)handle;
            pending.push_back(std::move(panel));
        }
        state->panels.clear();
        state->overlays.clear();
        pending.splice(pending.end(), state->panel_cleanup_pending);
    }

    sao_sdk_status_t cleanup_status = SAO_SDK_OK;
    std::list<PanelEntry> failed;
    for (auto current = pending.begin(); current != pending.end();) {
        auto candidate = current++;
        const auto status = candidate->ui_panel == nullptr
                                ? SAO_SDK_OK
                                : unregister_native_panel(candidate->ui_panel);
        if (status != SAO_SDK_OK) {
            if (cleanup_status == SAO_SDK_OK)
                cleanup_status = status;
            failed.splice(failed.end(), pending, candidate);
            continue;
        }
        destroy_panel_resources(*candidate);
        pending.erase(candidate);
    }
    if (!failed.empty()) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->panel_cleanup_pending.splice(state->panel_cleanup_pending.end(), failed);
    }
    return cleanup_status;
}

const SaoSdkUiTable* make_ui_table() {
    static const SaoSdkUiTable table = {
        // Legacy JSON path retained for scripting-language ui_spec consumers.
        ui_register_panel_boundary,
        ui_set_panel_spec_boundary,
        ui_set_overlay_boundary,
        ui_register_render_hook_legacy_boundary,
        ui_unregister_render_hook_legacy_boundary,
        // Typed render-clock hook.
        ui_register_render_hook_clock_boundary,
        ui_request_redraw_boundary,
        // Descriptor-based panel and widget CRUD.
        ui_register_ui_panel_boundary,
        ui_unregister_ui_panel_boundary,
        ui_panel_add_widget_boundary,
        ui_panel_update_widget_boundary,
        ui_panel_remove_widget_boundary,
    };
    return &table;
}

void test_fail_next_panel_state_insertion() noexcept {
    g_fail_next_panel_state_insertion.store(true);
}

void test_fail_next_panel_unregister(sao_sdk_status_t status) noexcept {
    g_fail_next_panel_unregister_status.store(status);
}

// ─── Public free-function wrappers ──────────────────────────────────

} // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_ui_panel(
    const struct SaoSdkContext* ctx, const struct SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->register_ui_panel == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->register_ui_panel(lease.state(), descriptor, out_panel);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_ui_panel(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->unregister_ui_panel == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->unregister_ui_panel(lease.state(), panel);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_add_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel,
    const struct SaoSdkWidgetSpec* widget_spec, sao_sdk_ui_widget_t* out_widget) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_add_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_add_widget(lease.state(), panel, widget_spec, out_widget);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_update_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget,
    const struct SaoSdkWidgetSpec* widget_spec) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_update_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_update_widget(lease.state(), panel, widget, widget_spec);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_remove_widget(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, sao_sdk_ui_widget_t widget) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->panel_remove_widget == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->panel_remove_widget(lease.state(), panel, widget);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook(
    const struct SaoSdkContext* ctx, int32_t hook_point, sao_sdk_render_hook_callback_t callback,
    void* user_data, sao_sdk_hook_token_t* out_hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr ||
            public_context->ui->register_render_hook_clock == nullptr) {
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        }
        return public_context->ui->register_render_hook_clock(
            lease.state(), hook_point, callback, user_data, out_hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook_ex(
    const struct SaoSdkContext* ctx, const struct SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return sao_sdk_internal::provider_render_register_ex(
            lease.state(), spec, callback, user_data, out_hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_render_hook(const struct SaoSdkContext* ctx, sao_sdk_hook_token_t hook_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        return sao_sdk_internal::make_ui_table()->unregister_render_hook(lease.state(),
                                                                         hook_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->request_redraw == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        auto* state = lease.state();
        std::string surface;
        if (panel != nullptr) {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto found = state->panels.find(panel);
            if (found == state->panels.end())
                return SAO_SDK_ERR_NOT_FOUND;
            if (found->second.unregistering)
                return SAO_SDK_ERR_BUSY;
            surface = found->second.panel_id;
        }
        return public_context->ui->request_redraw(lease.state(),
                                                  surface.empty() ? nullptr : surface.c_str());
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw_surface(const struct SaoSdkContext* ctx, const char* surface_id_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->ui == nullptr || public_context->ui->request_redraw == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->ui->request_redraw(lease.state(), surface_id_utf8);
    });
}

// ─── Test-only observability hooks ──────────────────────────────────
//
// Exported so the demo test file can inspect the SDK state without
// grubbing through the ctx_impl.  Only linked in tests — production
// builds strip these via the linker's dead-code elimination.

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_panel_state_insertion(void) {
    sao_sdk_internal::test_fail_next_panel_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_unregister(sao_sdk_status_t status) {
    sao_sdk_internal::test_fail_next_panel_unregister(status);
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_widget_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.widgets.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_canvas_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.canvases.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API uint64_t SAO_SDK_CALL
sao_sdk_test_panel_redraw_count(const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    try {
        if (panel == nullptr)
            return 0;
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end())
            return 0;
        return it->second.redraw_count;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_panel_invoke_action(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel, const char* action_key_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    if (panel == nullptr || action_key_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (panel == nullptr || action_key_utf8 == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        auto* state = lease.state();
        sao_sdk_panel_action_callback_t callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            const auto it = state->panels.find(panel);
            if (it == state->panels.end())
                return SAO_SDK_ERR_NOT_FOUND;
            callback = it->second.legacy_action_cb;
            user_data = it->second.legacy_action_user_data;
        }
        if (callback == nullptr)
            return SAO_SDK_ERR_UNSUPPORTED;
        sao_sdk_internal::PluginCallbackLease callback_lease(state);
        if (!callback_lease)
            return SAO_SDK_ERR_BUSY;
        return sao_sdk_internal::invoke_void_callback_barrier(
            [&] { callback(action_key_utf8, nullptr, 0, user_data); });
    });
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_render_hook_count(const struct SaoSdkContext* ctx) {
    try {
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        auto* state = lease.state();
        std::lock_guard<std::mutex> lk(state->mu);
        return state->render_hooks.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_cleanup_pending_count(const struct SaoSdkContext* ctx) {
    try {
        sao_sdk_internal::ContextApiLease lease(ctx);
        if (!lease)
            return 0;
        std::lock_guard<std::mutex> lock(lease.state()->mu);
        return lease.state()->panel_cleanup_pending.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fire_render_hook(int32_t hook_point, const struct SaoSdkRenderHookPayload* payload) {
    try {
        SaoSdkRenderHookPayload local{};
        if (payload != nullptr)
            local = *payload;
        sao_sdk_internal::fire_render_hook_test(hook_point, local);
    } catch (...) {
    }
}
#endif
