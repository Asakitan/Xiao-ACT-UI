// SAO Auto — Wave 7 SDK UI wire.
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

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

// Include the specific widget headers directly.  The umbrella
// (sao/ui/widget_kit.h) re-declares an extended kind enum whose
// values collide with the legacy d2d_widgets.h enum — pulling it in
// alongside widget_input/text/data would double-define several
// enumerators.
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_text.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/d2d_widgets.h"

extern "C" void SAO_UI_CALL sao_ui_widget_text_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_input_family_destroy(
    sao_ui_widget_handle_t handle);
extern "C" void SAO_UI_CALL sao_ui_widget_data_family_destroy(
    sao_ui_widget_handle_t handle);

namespace sao_sdk_internal {
namespace {

// ─── Widget instantiation helper ─────────────────────────────────────
//
// Translates SaoSdkWidgetSpec into the widget-kit call for the target
// kind.  Returns a widget handle (may be null on unimplemented kinds).
sao_ui_widget_handle_t create_widget_for_kind(const SaoSdkWidgetSpec& spec) {
    sao_ui_widget_handle_t handle = nullptr;
    switch (spec.kind) {
        case SAO_SDK_UI_WIDGET_LABEL: {
            SaoUiLabelSpec ls{};
            ls.text_utf8    = spec.text_utf8;
            ls.fg_argb      = 0;   // theme
            ls.bg_argb      = 0;
            ls.font_slot    = SAO_UI_FONT_CJK;
            ls.font_size_px = 14;
            ls.font_weight  = SAO_UI_WEIGHT_NORMAL;
            ls.align        = SAO_UI_ALIGN_LEFT;
            ls.anchor       = SAO_UI_ANCHOR_W;
            ls.max_lines    = 0;
            ls.wrap         = false;
            (void)sao_ui_label_create(nullptr, &ls, &handle);
            break;
        }
        case SAO_SDK_UI_WIDGET_BUTTON: {
            SaoUiButtonSpec bs{};
            bs.text_utf8 = spec.text_utf8;
            bs.kind      = SAO_UI_BTN_NORMAL;
            bs.radius_px = 6;
            bs.pad_x_px  = 12;
            bs.pad_y_px  = 6;
            (void)sao_ui_button_create(nullptr, &bs, &handle);
            break;
        }
        case SAO_SDK_UI_WIDGET_PROGRESS_BAR: {
            SaoUiProgressBarSpec ps{};
            ps.value     = spec.value;
            ps.max_value = spec.max_value <= 0.0f ? 1.0f : spec.max_value;
            ps.style     = SAO_UI_PROGRESS_FLAT;
            ps.radius_px = 4;
            (void)sao_ui_progress_bar_create(nullptr, &ps, &handle);
            break;
        }
        case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:
        case SAO_SDK_UI_WIDGET_TABLE:
        case SAO_SDK_UI_WIDGET_STATUS_BADGE:
        case SAO_SDK_UI_WIDGET_TEXT_FIELD:
        case SAO_SDK_UI_WIDGET_CHECKBOX:
        case SAO_SDK_UI_WIDGET_DIVIDER:
        case SAO_SDK_UI_WIDGET_ICON:
        default:
            // Generic path — the legacy widget kinds go through
            // sao_ui_widget_create() so unknown-yet kinds still get a
            // valid handle back.  Callers can populate the widget via
            // sao_ui_widget_apply_props() later.
            int32_t native_kind = SAO_UI_WIDGET_ROUNDED_PANEL;
            switch (spec.kind) {
                case SAO_SDK_UI_WIDGET_TABLE:        native_kind = SAO_UI_WIDGET_TABLE; break;
                case SAO_SDK_UI_WIDGET_ROUNDED_PANEL:native_kind = SAO_UI_WIDGET_ROUNDED_PANEL; break;
                case SAO_SDK_UI_WIDGET_STATUS_BADGE: native_kind = SAO_UI_WIDGET_STATUS_BADGE; break;
                case SAO_SDK_UI_WIDGET_TEXT_FIELD:  native_kind = SAO_UI_WIDGET_INPUT; break;
                case SAO_SDK_UI_WIDGET_CHECKBOX:    native_kind = SAO_UI_WIDGET_CHECKBOX; break;
                case SAO_SDK_UI_WIDGET_DIVIDER:     native_kind = SAO_UI_WIDGET_DIVIDER; break;
                case SAO_SDK_UI_WIDGET_ICON:        native_kind = SAO_UI_WIDGET_ICON; break;
                default: break;
            }
            (void)sao_ui_widget_create(native_kind, nullptr, &handle);
            break;
    }
    return handle;
}

void update_widget_for_kind(sao_ui_widget_handle_t widget,
                            const SaoSdkWidgetSpec& spec) {
    if (widget == nullptr) return;
    switch (spec.kind) {
        case SAO_SDK_UI_WIDGET_LABEL:
            if (spec.text_utf8 != nullptr) {
                (void)sao_ui_label_set_text(widget, spec.text_utf8);
            }
            break;
        case SAO_SDK_UI_WIDGET_BUTTON:
            if (spec.text_utf8 != nullptr) {
                (void)sao_ui_button_set_text(widget, spec.text_utf8);
            }
            break;
        case SAO_SDK_UI_WIDGET_PROGRESS_BAR:
            (void)sao_ui_progress_bar_set_value(widget, spec.value);
            if (spec.max_value > 0.0f) {
                (void)sao_ui_progress_bar_set_max(widget, spec.max_value);
            }
            break;
        default:
            if (spec.props_json_utf8 != nullptr && spec.props_len > 0) {
                (void)sao_ui_widget_apply_props(widget,
                                                spec.props_json_utf8,
                                                spec.props_len);
            }
            break;
    }
}

void clear_legacy_canvases(PanelEntry& panel) {
    for (const auto canvas : panel.canvases) {
        sao_ui_script_canvas_destroy(canvas);
    }
    panel.canvases.clear();
}

sao_sdk_status_t materialize_legacy_canvas(PanelEntry& panel,
                                            const uint8_t* spec_json_utf8,
                                            size_t spec_len) {
    if (spec_json_utf8 == nullptr || spec_len == 0) return SAO_SDK_OK;
    const std::string spec(reinterpret_cast<const char*>(spec_json_utf8), spec_len);
    if (spec.find("\"canvas\"") == std::string::npos) return SAO_SDK_OK;

    SaoUiScriptCanvasSpec canvas_spec{};
    canvas_spec.width_px = 640;
    canvas_spec.height_px = 360;
    canvas_spec.antialias = true;
    canvas_spec.retain_ops_between_frames = true;
    canvas_spec.max_ops_per_frame = 4000;
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t canvas = nullptr;
    const sao_status_t canvas_rc = sao_ui_script_canvas_create(
        nullptr, &canvas_spec, &widget, &canvas);
    if (canvas_rc != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(canvas_rc);

    SaoUiLayoutSpec layout{};
    layout.fixed_width_px = canvas_spec.width_px;
    layout.fixed_height_px = canvas_spec.height_px;
    layout.hit_testable = true;
    SaoUiBodyMutation mutation{};
    mutation.kind = SAO_UI_BODY_ADD_WIDGET;
    mutation.widget = widget;
    mutation.spec = &layout;
    const sao_status_t panel_rc = sao_ui_panel_update_body(panel.ui_body, &mutation, 1);
    if (panel_rc != SAO_STATUS_OK) {
        sao_ui_script_canvas_destroy(canvas);
        return static_cast<sao_sdk_status_t>(panel_rc);
    }
    panel.canvases.push_back(canvas);
    return SAO_SDK_OK;
}

// ─── UI table vtable functions ───────────────────────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(
    void* ctx_impl,
    const SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel);

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(
    void* ctx_impl, sao_sdk_ui_panel_t panel);

sao_sdk_status_t SAO_SDK_CALL ui_register_panel(
    void* ctx_impl, const char* panel_id_utf8, const char* title_utf8,
    const uint8_t* initial_spec_json_utf8, size_t spec_len,
    sao_sdk_panel_action_callback_t action_cb, void* action_user_data,
    sao_sdk_ui_panel_t* out_panel) {
    if (out_panel == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
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

    const sao_sdk_status_t register_rc =
        ui_register_ui_panel(ctx_impl, &descriptor, out_panel);
    if (register_rc != SAO_SDK_OK) return register_rc;

    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(*out_panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_HANDLE_INVALID;
        it->second.legacy_action_cb = action_cb;
        it->second.legacy_action_user_data = action_user_data;
        body = it->second.ui_body;
    }
    if (initial_spec_json_utf8 != nullptr) {
        const sao_status_t spec_rc = sao_ui_panel_body_set_spec(
            body, initial_spec_json_utf8, spec_len);
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
                canvas_rc = materialize_legacy_canvas(
                    it->second, initial_spec_json_utf8, spec_len);
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

sao_sdk_status_t SAO_SDK_CALL ui_set_panel_spec(
    void* ctx_impl, sao_sdk_ui_panel_t panel, const uint8_t* spec_json_utf8,
    size_t spec_len) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    sao_ui_panel_body_handle_t body = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        body = it->second.ui_body;
    }
    const sao_status_t spec_rc = sao_ui_panel_body_set_spec(body, spec_json_utf8, spec_len);
    if (spec_rc != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(spec_rc);
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->panels.find(panel);
    if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
    clear_legacy_canvases(it->second);
    return materialize_legacy_canvas(it->second, spec_json_utf8, spec_len);
}

sao_sdk_status_t SAO_SDK_CALL ui_set_overlay(
    void* ctx_impl, const char* surface_id_utf8,
    const uint8_t* spec_json_utf8, size_t spec_len) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
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
        if (clear_status != SAO_SDK_OK) return clear_status;
    }
    SaoSdkOverlaySpec spec{};
    spec.surface_id_utf8 = surface_id_utf8;
    spec.spec_json_utf8 = spec_json_utf8;
    spec.spec_len = spec_len;
    sao_sdk_overlay_token_t overlay = 0;
    return provider_overlay_set(state, &spec, &overlay);
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_legacy(
    void* ctx_impl, const char* surface_id_utf8, float priority,
    void* hook_fn, void* hook_user_data,
    sao_sdk_hook_token_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0' ||
        hook_fn == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    (void)priority;
    (void)hook_user_data;
    return SAO_SDK_ERR_UNSUPPORTED;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_render_hook_legacy(
    void* ctx_impl, sao_sdk_hook_token_t token) {
    return provider_render_unregister(cast_ctx(ctx_impl), token);
}

sao_sdk_status_t SAO_SDK_CALL ui_request_redraw(
    void* ctx_impl, const char* surface_id_utf8) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    const char* surface = surface_id_utf8 == nullptr
                              ? SAO_ENGINE_ALL_SURFACES
                              : surface_id_utf8;
    const auto status = provider_request_redraw(state, surface);
    if (status != SAO_SDK_OK) return status;
    std::lock_guard<std::mutex> lk(state->mu);
    for (auto& [handle, panel] : state->panels) {
        (void)handle;
        if (surface_id_utf8 == nullptr || panel.panel_id == surface_id_utf8) {
            ++panel.redraw_count;
        }
    }
    return SAO_SDK_OK;
}

// ─── Wave 7 append-only fields ──────────────────────────────────────

sao_sdk_status_t SAO_SDK_CALL ui_register_ui_panel(
    void* ctx_impl,
    const SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) {
    if (out_panel == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_panel = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (descriptor == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (descriptor->panel_id_utf8 == nullptr ||
        descriptor->panel_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    // Build a full SaoPanelDescriptor from the SDK subset.
    SaoPanelDescriptor full{};
    full.panel_id_utf8       = descriptor->panel_id_utf8;
    full.title_utf8          = descriptor->title_utf8;
    full.anchor              = SAO_UI_PANEL_ANCHOR_ABSOLUTE;
    full.default_x_px        = descriptor->default_x_px;
    full.default_y_px        = descriptor->default_y_px;
    full.default_width_px    = descriptor->default_width_px;
    full.default_height_px   = descriptor->default_height_px;
    full.min_width_px        = descriptor->min_width_px;
    full.min_height_px       = descriptor->min_height_px;
    full.max_width_px        = 0;
    full.max_height_px       = 0;
    full.movable             = descriptor->movable;
    full.resizable           = descriptor->resizable;
    full.show_titlebar       = descriptor->show_titlebar;
    full.show_close_button   = descriptor->show_close_button;
    full.visible             = descriptor->visible;
    full.remember_geometry   = descriptor->remember_geometry;
    full.modal               = descriptor->modal;
    full.overlay_style       = descriptor->overlay_style;
    full.z_class             = descriptor->z_class;
    full.z_within_class      = descriptor->z_within_class;
    full.initial_opacity     = descriptor->initial_opacity;

    auto& rt = SharedRuntime::instance();
    sao_ui_panel_handle_t      ui_panel = nullptr;
    sao_ui_panel_body_handle_t ui_body  = nullptr;
    const sao_status_t rc = sao_ui_panel_register(rt.compositor, &full,
                                                   &ui_panel, &ui_body);
    if (rc != SAO_STATUS_OK) {
        if (rc == SAO_STATUS_ERR_ALREADY_EXISTS) {
            return SAO_SDK_ERR_ALREADY_EXISTS;
        }
        if (rc == SAO_STATUS_ERR_INVALID_ARGUMENT) {
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        }
        return static_cast<sao_sdk_status_t>(rc);
    }

    PanelEntry entry;
    entry.sdk_handle = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
    entry.ui_panel   = ui_panel;
    entry.ui_body    = ui_body;
    entry.panel_id   = descriptor->panel_id_utf8;

    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->panels.emplace(entry.sdk_handle, std::move(entry));
    }
    *out_panel = reinterpret_cast<sao_sdk_ui_panel_t>(ui_panel);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_unregister_ui_panel(
    void* ctx_impl, sao_sdk_ui_panel_t panel) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)  return SAO_SDK_ERR_INVALID_ARGUMENT;

    sao_ui_panel_handle_t ui_panel = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        ui_panel = it->second.ui_panel;
        clear_legacy_canvases(it->second);
        for (const auto& widget : it->second.widgets) {
            destroy_widget_for_kind(widget.kind, widget.ui_widget);
        }
        for (auto overlay = state->overlays.begin(); overlay != state->overlays.end();) {
            if (overlay->second == panel) {
                overlay = state->overlays.erase(overlay);
            } else {
                ++overlay;
            }
        }
        state->panels.erase(it);
    }
    if (ui_panel != nullptr) {
        (void)sao_ui_panel_unregister(ui_panel);
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_add_widget(
    void* ctx_impl,
    sao_sdk_ui_panel_t panel,
    const SaoSdkWidgetSpec* widget_spec,
    sao_sdk_ui_widget_t* out_widget) {
    if (out_widget != nullptr) *out_widget = nullptr;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)      return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)      return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;

    // Create the underlying platform widget outside the lock — some
    // widget_kit paths take their own locks.
    sao_ui_widget_handle_t ui_widget = create_widget_for_kind(*widget_spec);
    if (ui_widget == nullptr) return SAO_SDK_ERR_UNSUPPORTED;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) {
        destroy_widget_for_kind(widget_spec->kind, ui_widget);
        return SAO_SDK_ERR_NOT_FOUND;
    }
    PanelEntry& pe = it->second;

    // Reject duplicate widget_id inside the same panel.
    if (widget_spec->widget_id_utf8 != nullptr &&
        widget_spec->widget_id_utf8[0] != '\0') {
        for (const auto& w : pe.widgets) {
            if (w.widget_id == widget_spec->widget_id_utf8) {
                destroy_widget_for_kind(widget_spec->kind, ui_widget);
                return SAO_SDK_ERR_ALREADY_EXISTS;
            }
        }
    }

    // Batch mutation so the platform sees one dirty-rects snapshot.
    SaoUiLayoutSpec layout{};
    layout.fixed_width_px  = widget_spec->width_px;
    layout.fixed_height_px = widget_spec->height_px;
    layout.absolute_x_px   = widget_spec->x_px;
    layout.absolute_y_px   = widget_spec->y_px;
    layout.absolute_z      = widget_spec->z_order;
    layout.hit_testable    = true;

    sao_ui_layout_node_handle_t new_node = nullptr;
    SaoUiBodyMutation mut{};
    mut.kind          = SAO_UI_BODY_ADD_WIDGET;
    mut.target        = nullptr;     // root of body
    mut.widget        = ui_widget;
    mut.spec          = &layout;
    mut.out_new_node  = &new_node;
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK) {
        destroy_widget_for_kind(widget_spec->kind, ui_widget);
        return static_cast<sao_sdk_status_t>(update_rc);
    }

    WidgetEntry we;
    we.sdk_handle  = reinterpret_cast<sao_sdk_ui_widget_t>(
        reinterpret_cast<uintptr_t>(&pe) + pe.next_widget_id);
    we.ui_widget   = ui_widget;
    pe.next_widget_id += 8;
    we.widget_id   = (widget_spec->widget_id_utf8 == nullptr)
                       ? std::string() : widget_spec->widget_id_utf8;
    we.kind        = widget_spec->kind;
    we.layout_node = new_node;
    pe.widgets.push_back(std::move(we));

    if (out_widget != nullptr) {
        *out_widget = pe.widgets.back().sdk_handle;
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_update_widget(
    void* ctx_impl,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget,
    const SaoSdkWidgetSpec* widget_spec) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)      return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)      return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget == nullptr)     return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget_spec == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;

    WidgetEntry* target = nullptr;
    for (auto& w : pe.widgets) {
        if (w.sdk_handle == widget) { target = &w; break; }
    }
    if (target == nullptr) return SAO_SDK_ERR_NOT_FOUND;

    update_widget_for_kind(target->ui_widget, *widget_spec);

    // The layout-node handle produced by sao_ui_panel_update_body is a
    // synthetic value in the current slice — passing it as the update
    // target still logs the mutation in the panel body so callers see
    // one dirty-rects batch.
    SaoUiBodyMutation mut{};
    mut.kind = SAO_UI_BODY_UPDATE_WIDGET_PROPS;
    mut.target = target->layout_node;
    mut.props_json_utf8 = widget_spec->props_json_utf8;
    mut.props_len       = widget_spec->props_len;
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(update_rc);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_panel_remove_widget(
    void* ctx_impl,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)  return SAO_SDK_ERR_HANDLE_INVALID;
    if (panel == nullptr)  return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (widget == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
    PanelEntry& pe = it->second;

    auto pred = [widget](const WidgetEntry& w) { return w.sdk_handle == widget; };
    auto rem  = std::find_if(pe.widgets.begin(), pe.widgets.end(), pred);
    if (rem == pe.widgets.end()) return SAO_SDK_ERR_NOT_FOUND;

    SaoUiBodyMutation mut{};
    mut.kind = SAO_UI_BODY_REMOVE_NODE;
    mut.target = rem->layout_node;
    const sao_status_t update_rc = sao_ui_panel_update_body(pe.ui_body, &mut, 1);
    if (update_rc != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(update_rc);
    destroy_widget_for_kind(rem->kind, rem->ui_widget);
    pe.widgets.erase(rem);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL ui_register_render_hook_clock(
    void* ctx_impl,
    int32_t hook_point,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)     return SAO_SDK_ERR_HANDLE_INVALID;
    if (callback == nullptr)  return SAO_SDK_ERR_INVALID_ARGUMENT;

    return provider_render_register(state, hook_point, callback, user_data,
                                    out_token);
}

}  // namespace

void destroy_widget_for_kind(int32_t kind, sao_ui_widget_handle_t widget) {
    if (widget == nullptr) return;
    switch (kind) {
        case SAO_SDK_UI_WIDGET_LABEL:
            sao_ui_widget_text_family_destroy(widget);
            break;
        case SAO_SDK_UI_WIDGET_BUTTON:
            sao_ui_widget_input_family_destroy(widget);
            break;
        case SAO_SDK_UI_WIDGET_PROGRESS_BAR:
            sao_ui_widget_data_family_destroy(widget);
            break;
        default:
            sao_ui_widget_destroy(widget);
            break;
    }
}

const SaoSdkUiTable* make_ui_table() {
    static const SaoSdkUiTable table = {
        // Legacy JSON path (Wave 6 will populate the JSON normalizer).
        ui_register_panel,
        ui_set_panel_spec,
        ui_set_overlay,
        ui_register_render_hook_legacy,
        ui_unregister_render_hook_legacy,
        // Wave 7 typed render-clock hook.
        ui_register_render_hook_clock,
        ui_request_redraw,
        // Wave 7 descriptor + widget CRUD.
        ui_register_ui_panel,
        ui_unregister_ui_panel,
        ui_panel_add_widget,
        ui_panel_update_widget,
        ui_panel_remove_widget,
    };
    return &table;
}

// ─── Public free-function wrappers (Wave 7) ─────────────────────────

}  // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_ui_panel(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkPanelDescriptor* descriptor,
    sao_sdk_ui_panel_t* out_panel) {
    if (ctx == nullptr || ctx->ui == nullptr ||
        ctx->ui->register_ui_panel == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->ui->register_ui_panel(ctx->ctx_impl, descriptor, out_panel);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unregister_ui_panel(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    if (ctx == nullptr || ctx->ui == nullptr ||
        ctx->ui->unregister_ui_panel == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->ui->unregister_ui_panel(ctx->ctx_impl, panel);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_add_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    const struct SaoSdkWidgetSpec* widget_spec,
    sao_sdk_ui_widget_t* out_widget) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->ui->panel_add_widget(ctx->ctx_impl, panel, widget_spec,
                                     out_widget);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_update_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget,
    const struct SaoSdkWidgetSpec* widget_spec) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->ui->panel_update_widget(ctx->ctx_impl, panel, widget,
                                        widget_spec);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_panel_remove_widget(
    const struct SaoSdkContext* ctx,
    sao_sdk_ui_panel_t panel,
    sao_sdk_ui_widget_t widget) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->ui->panel_remove_widget(ctx->ctx_impl, panel, widget);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_render_hook(
    const struct SaoSdkContext* ctx,
    int32_t hook_point,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_hook_handle) {
    if (ctx == nullptr || ctx->ui == nullptr ||
        ctx->ui->register_render_hook_clock == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->ui->register_render_hook_clock(ctx->ctx_impl, hook_point,
                                                callback, user_data,
                                                out_hook_handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_register_render_hook_ex(
    const struct SaoSdkContext* ctx,
    const struct SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback,
    void* user_data,
    sao_sdk_hook_token_t* out_hook_handle) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_render_register_ex(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl), spec, callback, user_data,
        out_hook_handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_unregister_render_hook(
    const struct SaoSdkContext* ctx, sao_sdk_hook_token_t hook_handle) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    return sao_sdk_internal::make_ui_table()->unregister_render_hook(
        state, hook_handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_request_redraw(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    if (ctx == nullptr || ctx->ui == nullptr ||
        ctx->ui->request_redraw == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    std::string surface;
    if (panel != nullptr) {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto found = state->panels.find(panel);
        if (found == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        surface = found->second.panel_id;
    }
    return ctx->ui->request_redraw(ctx->ctx_impl,
                                   surface.empty() ? nullptr : surface.c_str());
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_request_redraw_surface(
    const struct SaoSdkContext* ctx,
    const char* surface_id_utf8) {
    if (ctx == nullptr || ctx->ui == nullptr ||
        ctx->ui->request_redraw == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->ui->request_redraw(ctx->ctx_impl, surface_id_utf8);
}

// ─── Test-only observability hooks ──────────────────────────────────
//
// Exported so the demo test file can inspect the SDK state without
// grubbing through the ctx_impl.  Only linked in tests — production
// builds strip these via the linker's dead-code elimination.

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_panel_widget_count(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    if (ctx == nullptr || panel == nullptr) return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return 0;
    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) return 0;
    return it->second.widgets.size();
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_panel_canvas_count(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    if (ctx == nullptr || panel == nullptr) return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return 0;
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->panels.find(panel);
    if (it == state->panels.end()) return 0;
    return it->second.canvases.size();
}

extern "C" SAO_SDK_API uint64_t SAO_SDK_CALL sao_sdk_test_panel_redraw_count(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel) {
    if (ctx == nullptr || panel == nullptr) return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return 0;
    std::lock_guard<std::mutex> lk(state->mu);
    auto it = state->panels.find(panel);
    if (it == state->panels.end()) return 0;
    return it->second.redraw_count;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_test_panel_invoke_action(const struct SaoSdkContext* ctx,
                                 sao_sdk_ui_panel_t panel,
                                 const char* action_key_utf8) {
    if (ctx == nullptr || panel == nullptr || action_key_utf8 == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    sao_sdk_panel_action_callback_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto it = state->panels.find(panel);
        if (it == state->panels.end()) return SAO_SDK_ERR_NOT_FOUND;
        callback = it->second.legacy_action_cb;
        user_data = it->second.legacy_action_user_data;
    }
    if (callback == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    callback(action_key_utf8, nullptr, 0, user_data);
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_render_hook_count(
    const struct SaoSdkContext* ctx) {
    if (ctx == nullptr) return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return 0;
    std::lock_guard<std::mutex> lk(state->mu);
    return state->render_hooks.size();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fire_render_hook(
    int32_t hook_point, const struct SaoSdkRenderHookPayload* payload) {
    SaoSdkRenderHookPayload local{};
    if (payload != nullptr) local = *payload;
    sao_sdk_internal::fire_render_hook_test(hook_point, local);
}
