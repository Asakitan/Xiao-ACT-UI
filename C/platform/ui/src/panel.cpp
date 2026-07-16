// SAO Auto — panel stub.  1:1 with `sao_panel_ui.py` + `sao_gui/panel.py`.

#include "sao/ui/panel.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct sao_ui_panel_s {
    sao_ui_compositor_handle_t compositor{};
    std::string id;
    std::string title;
    SaoPanelState state{};
    sao_ui_layout_tree_handle_t tree{};
    sao_ui_layout_node_handle_t root{};
    sao_ui_panel_action_callback_t action_callback{};
    void* action_user_data{};
    sao_ui_panel_event_callback_t event_callback{};
    void* event_user_data{};
    sao_ui_panel_render_fn_t render_callback{};
    void* render_user_data{};
    std::string spec_json;
    std::mutex mutex;
};

namespace {

std::mutex& panels_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<sao_ui_panel_s*>& panels() {
    static std::vector<sao_ui_panel_s*> values;
    return values;
}

void notify(sao_ui_panel_s* panel, int32_t event) {
    sao_ui_panel_event_callback_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::scoped_lock lock(panel->mutex);
        callback = panel->event_callback;
        user_data = panel->event_user_data;
    }
    if (callback != nullptr) callback(event, user_data);
}

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_create(
    sao_ui_compositor_handle_t compositor, const SaoPanelConfig* config,
    sao_ui_panel_handle_t* out_handle) {
    if (out_handle == nullptr || config == nullptr || config->panel_id_utf8 == nullptr || config->panel_id_utf8[0] == '\0' ||
        config->default_width <= 0 || config->default_height <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> registry_lock(panels_mutex());
    for (sao_ui_panel_s* existing : panels()) {
        if (existing->compositor == compositor && existing->id == config->panel_id_utf8) {
            if (config->single_instance) {
                *out_handle = existing;
                return SAO_STATUS_OK;
            }
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
    }
    auto* panel = new (std::nothrow) sao_ui_panel_s();
    if (panel == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    panel->compositor = compositor;
    panel->id = config->panel_id_utf8;
    panel->title = config->title_utf8 == nullptr ? "" : config->title_utf8;
    panel->state.visible = false;
    panel->state.x = config->default_x;
    panel->state.y = config->default_y;
    panel->state.width = std::max(config->min_width, config->default_width);
    panel->state.height = std::max(config->min_height, config->default_height);
    SaoUiLayoutSpec root_spec{};
    sao_ui_layout_spec_defaults(&root_spec);
    root_spec.fixed_width_px = panel->state.width;
    root_spec.fixed_height_px = panel->state.height;
    if (sao_ui_layout_tree_create(&panel->tree) != SAO_STATUS_OK ||
        sao_ui_layout_tree_set_root(panel->tree, SAO_UI_LAYOUT_VERTICAL, &root_spec, &panel->root) != SAO_STATUS_OK) {
        sao_ui_layout_tree_destroy(panel->tree);
        delete panel;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    panels().push_back(panel);
    *out_handle = panel;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_panel_destroy(sao_ui_panel_handle_t panel) {
    if (panel == nullptr) return;
    {
        std::lock_guard<std::mutex> registry_lock(panels_mutex());
        auto& entries = panels();
        entries.erase(std::remove(entries.begin(), entries.end(), panel), entries.end());
    }
    sao_ui_layout_tree_destroy(panel->tree);
    delete panel;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_spec(
    sao_ui_panel_handle_t panel, const uint8_t* spec_json_utf8, size_t spec_len) {
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0U)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(panel->mutex);
    panel->spec_json = spec_len == 0U ? "" : std::string(reinterpret_cast<const char*>(spec_json_utf8), spec_len);
    return sao_ui_layout_tree_invalidate_all(panel->tree);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_update_widget(
    sao_ui_panel_handle_t panel, const char* widget_id_utf8, const uint8_t* props_json_utf8, size_t props_len) {
    if (panel == nullptr || widget_id_utf8 == nullptr || (props_json_utf8 == nullptr && props_len != 0U)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_visible(
    sao_ui_panel_handle_t panel, bool visible) {
    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    bool changed = false;
    {
        std::scoped_lock lock(panel->mutex);
        changed = panel->state.visible != visible;
        panel->state.visible = visible;
    }
    if (changed) notify(panel, visible ? SAO_UI_PANEL_EVENT_SHOW : SAO_UI_PANEL_EVENT_HIDE);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_position(
    sao_ui_panel_handle_t panel, int32_t x, int32_t y) {
    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    { std::scoped_lock lock(panel->mutex); panel->state.x = x; panel->state.y = y; }
    notify(panel, SAO_UI_PANEL_EVENT_MOVE);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry(
    sao_ui_panel_handle_t panel, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (panel == nullptr || width <= 0 || height <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    { std::scoped_lock lock(panel->mutex); panel->state.x = x; panel->state.y = y; panel->state.width = width; panel->state.height = height; }
    SaoUiRect rect{0, 0, width, height};
    sao_ui_layout_arrange(panel->root, rect);
    notify(panel, SAO_UI_PANEL_EVENT_RESIZE);
    return SAO_STATUS_OK;
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL sao_ui_panel_layer(
    sao_ui_panel_handle_t) {
    return nullptr;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_get_layout_tree(
    sao_ui_panel_handle_t panel, sao_ui_layout_tree_handle_t* out_tree,
    sao_ui_layout_node_handle_t* out_root) {
    if (panel == nullptr || out_tree == nullptr || out_root == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_tree = panel->tree;
    *out_root = panel->root;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_rasterize(
    sao_ui_panel_handle_t panel, sao_ui_offscreen_raster_handle_t raster) {
    if (panel == nullptr || raster == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SaoPanelState state{};
    std::string title;
    sao_ui_panel_render_fn_t render = nullptr;
    void* render_user_data = nullptr;
    {
        std::scoped_lock lock(panel->mutex);
        state = panel->state;
        title = panel->title;
        render = panel->render_callback;
        render_user_data = panel->render_user_data;
    }
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_status_t status = sao_ui_paint_ctx_create_offscreen(raster, &context);
    if (status != SAO_STATUS_OK) return status;
    sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F, static_cast<float>(state.width), static_cast<float>(state.height), 0xe61d2430U);
    sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F, static_cast<float>(state.width), 24.0F, 0xff293a52U);
    if (!title.empty()) sao_ui_paint_ctx_draw_utf8(context, 6.0F, 6.0F, title.c_str(), 12.0F, 0xffffffffU);
    if (render != nullptr) render(context, 0.0F, 0.0F, static_cast<float>(state.width), static_cast<float>(state.height), render_user_data);
    sao_ui_paint_ctx_destroy(context);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_action_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_action_callback_t callback, void* user_data) {
    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(panel->mutex);
    panel->action_callback = callback;
    panel->action_user_data = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_event_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_event_callback_t callback, void* user_data) {
    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(panel->mutex);
    panel->event_callback = callback;
    panel->event_user_data = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_render_fn(
    sao_ui_panel_handle_t panel, sao_ui_panel_render_fn_t callback, void* user_data) {
    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(panel->mutex);
    panel->render_callback = callback;
    panel->render_user_data = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_get_state(
    sao_ui_panel_handle_t panel, SaoPanelState* out_state) {
    if (panel == nullptr || out_state == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::scoped_lock lock(panel->mutex);
    *out_state = panel->state;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_find_by_id(
    sao_ui_compositor_handle_t compositor, const char* panel_id_utf8,
    sao_ui_panel_handle_t* out_handle) {
    if (out_handle == nullptr || panel_id_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> registry_lock(panels_mutex());
    for (sao_ui_panel_s* panel : panels()) {
        if (panel->compositor == compositor && panel->id == panel_id_utf8) {
            *out_handle = panel;
            return SAO_STATUS_OK;
        }
    }
    *out_handle = nullptr;
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_enumerate(
    sao_ui_compositor_handle_t compositor, sao_ui_panel_handle_t* out_handles, size_t capacity,
    size_t* out_written) {
    if (out_written == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> registry_lock(panels_mutex());
    size_t total = 0;
    for (sao_ui_panel_s* panel : panels()) {
        if (panel->compositor != compositor) continue;
        if (out_handles != nullptr && total < capacity) out_handles[total] = panel;
        ++total;
    }
    *out_written = total;
    return out_handles != nullptr && capacity < total ? SAO_STATUS_ERR_BUFFER_TOO_SMALL : SAO_STATUS_OK;
}
