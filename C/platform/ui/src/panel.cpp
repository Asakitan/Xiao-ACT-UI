// SAO Auto — generic spec-driven panel backed by a compositor layer.

#include "sao/ui/panel.h"

#include "sao/engine/ui_spec.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;
constexpr int32_t kTitlebarHeight = 24;
std::atomic<int32_t> g_body_replace_failure_point{0};

struct OwnedWidget {
    sao_ui_widget_handle_t handle{};
    sao_ui_layout_node_handle_t node{};
    int32_t kind{};
    std::string id;
    std::string action;
    std::string action_args{"{}"};
    json props{json::object()};
    bool enabled{true};
    bool owns_handle{true};

    ~OwnedWidget() {
        if (owns_handle)
            sao_ui_widget_destroy(handle);
    }
};

struct PanelContent {
    sao_ui_layout_tree_handle_t tree{};
    sao_ui_layout_node_handle_t root{};
    SaoUiLayoutSpec root_spec{};
    std::vector<std::unique_ptr<OwnedWidget>> widgets;
    std::unordered_map<std::string, OwnedWidget*> by_id;
    std::unordered_map<sao_ui_widget_handle_t, OwnedWidget*> by_handle;
    std::mutex mutex;

    ~PanelContent() {
        sao_ui_layout_tree_destroy(tree);
    }
};

struct PanelFrame {
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
};

} // namespace

struct sao_ui_panel_s {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_layer_handle_t layer{};
    std::string id;
    std::string title;
    SaoPanelState state{};
    bool show_titlebar{true};
    int32_t min_width{};
    int32_t min_height{};
    int32_t max_width{};
    int32_t max_height{};
    std::shared_ptr<PanelContent> content;
    sao_ui_panel_action_callback_t action_callback{};
    void* action_user_data{};
    sao_ui_panel_event_callback_t event_callback{};
    void* event_user_data{};
    sao_ui_panel_render_fn_t render_callback{};
    void* render_user_data{};
    std::string spec_json;
    std::mutex mutex;
    std::recursive_mutex render_mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    size_t operations_in_flight{};
    size_t callbacks_in_flight{};
    std::array<uint64_t, 3> callback_generations{1, 1, 1};
    std::array<std::unordered_map<uint64_t, size_t>, 3> callback_in_flight_by_generation;
    bool accepting_operations{true};
    bool destroy_requested{};
    bool finalizing{};
    bool finalized{};
    bool stopping{};

    ~sao_ui_panel_s() {
        if (layer != nullptr) {
            (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            sao_ui_layer_destroy(layer);
        }
    }
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

std::vector<std::unique_ptr<sao_ui_panel_s>>& panel_storage() {
    static std::vector<std::unique_ptr<sao_ui_panel_s>> values;
    return values;
}

enum class CallbackKind : size_t { Action = 0, Event = 1, Render = 2 };

struct ActiveCallback {
    sao_ui_panel_s* panel{};
    CallbackKind kind{};
    uint64_t generation{};
    ActiveCallback* previous{};
};

thread_local ActiveCallback* active_callback = nullptr;

struct ActiveRender {
    sao_ui_panel_s* panel{};
    ActiveRender* previous{};
};

thread_local ActiveRender* active_render = nullptr;

bool callback_is_active(sao_ui_panel_s* panel, CallbackKind kind, uint64_t generation) {
    for (const ActiveCallback* active = active_callback; active != nullptr;
         active = active->previous) {
        if (active->panel == panel && active->kind == kind && active->generation == generation)
            return true;
    }
    return false;
}

bool panel_is_live_locked(sao_ui_panel_s* panel) {
    return std::ranges::find(panels(), panel) != panels().end();
}

bool claim_finalization(sao_ui_panel_s* panel) {
    std::lock_guard lock(panel->lifecycle_mutex);
    if (!panel->destroy_requested || panel->finalizing || panel->finalized ||
        panel->operations_in_flight != 0 || panel->callbacks_in_flight != 0) {
        return false;
    }
    panel->finalizing = true;
    return true;
}

void finalize_panel(sao_ui_panel_s* panel) noexcept {
    try {
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            layer = std::exchange(panel->layer, nullptr);
            panel->content.reset();
            panel->action_callback = nullptr;
            panel->action_user_data = nullptr;
            panel->event_callback = nullptr;
            panel->event_user_data = nullptr;
            panel->render_callback = nullptr;
            panel->render_user_data = nullptr;
        }
        if (layer != nullptr) {
            (void)sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            (void)sao_ui_layer_set_input_enabled(layer, false);
            sao_ui_layer_destroy(layer);
        }
    } catch (...) {
    }
    {
        std::lock_guard lock(panel->lifecycle_mutex);
        panel->finalized = true;
        panel->finalizing = false;
    }
    panel->lifecycle_cv.notify_all();
}

void finalize_if_ready(sao_ui_panel_s* panel) noexcept {
    if (claim_finalization(panel))
        finalize_panel(panel);
}

class PanelOperation {
  public:
    explicit PanelOperation(sao_ui_panel_s* panel) : panel_(panel) {
        if (panel_ == nullptr)
            return;
        try {
            std::lock_guard registry_lock(panels_mutex());
            if (!panel_is_live_locked(panel_))
                return;
            std::lock_guard lifecycle_lock(panel_->lifecycle_mutex);
            if (!panel_->accepting_operations)
                return;
            ++panel_->operations_in_flight;
            acquired_ = true;
        } catch (...) {
        }
    }

    PanelOperation(const PanelOperation&) = delete;
    PanelOperation& operator=(const PanelOperation&) = delete;

    ~PanelOperation() {
        if (!acquired_)
            return;
        {
            std::lock_guard lock(panel_->lifecycle_mutex);
            --panel_->operations_in_flight;
        }
        panel_->lifecycle_cv.notify_all();
        finalize_if_ready(panel_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    sao_ui_panel_s* panel_{};
    bool acquired_{};
};

class PanelCallbackLease {
  public:
    PanelCallbackLease(sao_ui_panel_s* panel, CallbackKind kind) : panel_(panel), kind_(kind) {
        try {
            std::lock_guard lock(panel_->lifecycle_mutex);
            if (!panel_->accepting_operations)
                return;
            const size_t index = static_cast<size_t>(kind_);
            generation_ = panel_->callback_generations[index];
            {
                std::lock_guard state_lock(panel_->mutex);
                if (kind_ == CallbackKind::Action) {
                    action_ = panel_->action_callback;
                    user_data_ = panel_->action_user_data;
                } else if (kind_ == CallbackKind::Event) {
                    event_ = panel_->event_callback;
                    user_data_ = panel_->event_user_data;
                } else {
                    render_ = panel_->render_callback;
                    user_data_ = panel_->render_user_data;
                }
            }
            ++panel_->callback_in_flight_by_generation[index][generation_];
            ++panel_->callbacks_in_flight;
            active_marker_ = {panel_, kind_, generation_, active_callback};
            active_callback = &active_marker_;
            acquired_ = true;
        } catch (...) {
        }
    }

    PanelCallbackLease(const PanelCallbackLease&) = delete;
    PanelCallbackLease& operator=(const PanelCallbackLease&) = delete;

    ~PanelCallbackLease() {
        if (!acquired_)
            return;
        active_callback = active_marker_.previous;
        {
            std::lock_guard lock(panel_->lifecycle_mutex);
            const size_t index = static_cast<size_t>(kind_);
            auto& by_generation = panel_->callback_in_flight_by_generation[index];
            auto found = by_generation.find(generation_);
            if (found != by_generation.end() && --found->second == 0)
                by_generation.erase(found);
            --panel_->callbacks_in_flight;
        }
        panel_->lifecycle_cv.notify_all();
        finalize_if_ready(panel_);
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

    sao_ui_panel_action_callback_t action() const noexcept {
        return action_;
    }

    sao_ui_panel_event_callback_t event() const noexcept {
        return event_;
    }

    sao_ui_panel_render_fn_t render() const noexcept {
        return render_;
    }

    void* user_data() const noexcept {
        return user_data_;
    }

  private:
    sao_ui_panel_s* panel_{};
    CallbackKind kind_{};
    uint64_t generation_{};
    sao_ui_panel_action_callback_t action_{};
    sao_ui_panel_event_callback_t event_{};
    sao_ui_panel_render_fn_t render_{};
    void* user_data_{};
    ActiveCallback active_marker_{};
    bool acquired_{};
};

void wait_for_callback_generation(sao_ui_panel_s* panel, CallbackKind kind, uint64_t generation) {
    if (callback_is_active(panel, kind, generation))
        return;
    const size_t index = static_cast<size_t>(kind);
    std::unique_lock lock(panel->lifecycle_mutex);
    panel->lifecycle_cv.wait(
        lock, [&] { return !panel->callback_in_flight_by_generation[index].contains(generation); });
}

class PanelRenderGuard {
  public:
    explicit PanelRenderGuard(sao_ui_panel_s* panel) : panel_(panel) {
        for (const ActiveRender* active = active_render; active != nullptr;
             active = active->previous) {
            if (active->panel == panel_)
                return;
        }
        render_lock_ = std::unique_lock<std::recursive_mutex>(panel_->render_mutex);
        marker_ = {panel_, active_render};
        active_render = &marker_;
        acquired_ = true;
    }

    PanelRenderGuard(const PanelRenderGuard&) = delete;
    PanelRenderGuard& operator=(const PanelRenderGuard&) = delete;

    ~PanelRenderGuard() {
        if (acquired_)
            active_render = marker_.previous;
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

  private:
    sao_ui_panel_s* panel_{};
    std::unique_lock<std::recursive_mutex> render_lock_;
    ActiveRender marker_{};
    bool acquired_{};
};

int32_t content_top(const sao_ui_panel_s& panel) {
    return panel.show_titlebar ? kTitlebarHeight : 0;
}

int32_t clamp_dimension(int32_t value, int32_t minimum, int32_t maximum) {
    value = std::max(value, std::max(1, minimum));
    return maximum > 0 ? std::min(value, maximum) : value;
}

std::string normalized_type(const json& node) {
    if (!node.is_object() || !node.contains("type"))
        return "text";
    std::string type;
    if (node["type"].is_string())
        type = node["type"].get<std::string>();
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return type;
}

void restore_node_ids(const json& source_nodes, json& normalized_nodes) {
    if (!source_nodes.is_array() || !normalized_nodes.is_array())
        return;
    size_t source_index = 0;
    for (auto& normalized : normalized_nodes) {
        if (!normalized.is_object())
            continue;
        const std::string target_type = normalized_type(normalized);
        const json* source = nullptr;
        while (source_index < source_nodes.size()) {
            const json& candidate = source_nodes[source_index++];
            if (normalized_type(candidate) == target_type) {
                source = &candidate;
                break;
            }
        }
        if (source == nullptr || !source->is_object())
            continue;
        if (source->contains("id") && (*source)["id"].is_string()) {
            std::string id = (*source)["id"].get<std::string>();
            size_t byte = 0;
            size_t codepoints = 0;
            while (byte < id.size() && codepoints < 80u) {
                const unsigned char lead = static_cast<unsigned char>(id[byte]);
                size_t width = 1;
                if ((lead & 0xe0U) == 0xc0U)
                    width = 2;
                else if ((lead & 0xf0U) == 0xe0U)
                    width = 3;
                else if ((lead & 0xf8U) == 0xf0U)
                    width = 4;
                if (byte + width > id.size())
                    break;
                byte += width;
                ++codepoints;
            }
            if (byte < id.size())
                id.resize(byte);
            normalized["id"] = std::move(id);
        }
        if (source->contains("children") && normalized.contains("children")) {
            restore_node_ids((*source)["children"], normalized["children"]);
        }
    }
}

json source_nodes_for(const json& input) {
    if (input.is_array())
        return input;
    if (!input.is_object())
        return json::array({input});
    if (input.contains("nodes") && input["nodes"].is_array()) {
        return input["nodes"];
    }
    if (input.contains("children") && input["children"].is_array()) {
        return input["children"];
    }
    return json::array({input});
}

sao_status_t normalize_spec(const uint8_t* bytes, size_t length, json* out_spec,
                            std::string* out_serialized) {
    if (out_spec == nullptr || out_serialized == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    json input;
    try {
        input = length == 0 ? json::object() : json::parse(bytes, bytes + length);
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (input.is_object() && input.value("version", 0) == SAO_UI_SPEC_VERSION &&
        input.contains("nodes") && input["nodes"].is_array()) {
        *out_spec = std::move(input);
        *out_serialized = out_spec->dump();
        return SAO_STATUS_OK;
    }

    const std::string envelope = json{{"spec", input}, {"title", ""}}.dump();
    size_t required = 0;
    sao_status_t status = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size(), nullptr, 0, &required);
    if (status != SAO_STATUS_OK)
        return status;
    std::vector<uint8_t> normalized(required);
    status = sao_engine_ui_spec_normalize(reinterpret_cast<const uint8_t*>(envelope.data()),
                                          envelope.size(), normalized.data(), normalized.size(),
                                          &required);
    if (status != SAO_STATUS_OK)
        return status;
    try {
        out_serialized->assign(reinterpret_cast<const char*>(normalized.data()), required);
        *out_spec = json::parse(*out_serialized);
        if (out_spec->contains("nodes")) {
            const json source_nodes = source_nodes_for(input);
            restore_node_ids(source_nodes, (*out_spec)["nodes"]);
            *out_serialized = out_spec->dump();
        }
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

int32_t widget_kind(const std::string& type) {
    if (type == "button")
        return SAO_UI_WIDGET_ACTION_BUTTON;
    if (type == "badge")
        return SAO_UI_WIDGET_STATUS_BADGE;
    if (type == "bar")
        return SAO_UI_WIDGET_BAR;
    if (type == "divider")
        return SAO_UI_WIDGET_DIVIDER;
    if (type == "input")
        return SAO_UI_WIDGET_INPUT;
    if (type == "slider")
        return SAO_UI_WIDGET_SLIDER;
    if (type == "table")
        return SAO_UI_WIDGET_TABLE;
    return SAO_UI_WIDGET_TEXT;
}

json make_widget_props(const json& node, const std::string& type) {
    json props = node;
    if (type == "bar" && node.contains("pct"))
        props["value"] = node["pct"];
    if (type == "kv") {
        props["text"] =
            node.value("label", std::string()) + ": " + node.value("value", std::string());
    } else if (type == "button") {
        props["text"] = node.value("label", std::string());
        props["enabled"] = !node.value("disabled", false);
    } else if (type == "input") {
        props["text"] = node.value("value", std::string());
    } else if (type == "table") {
        props["text"] = node.value("title", std::string());
    } else if (type == "bar" && !node.value("caption", std::string()).empty()) {
        props["text"] = node.value("caption", std::string());
    }
    return props;
}

sao_status_t append_nodes(PanelContent& content, sao_ui_layout_node_handle_t parent,
                          const json& nodes) {
    if (!nodes.is_array())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    for (const auto& node : nodes) {
        if (!node.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const std::string type = node.value("type", std::string());
        if (node.contains("children") && node["children"].is_array()) {
            SaoUiLayoutSpec spec{};
            sao_ui_layout_spec_defaults(&spec);
            spec.gap_px = 2;
            sao_ui_layout_node_handle_t child = nullptr;
            const int32_t mode = type == "row" ? SAO_UI_LAYOUT_HORIZONTAL : SAO_UI_LAYOUT_VERTICAL;
            sao_status_t status = sao_ui_layout_node_add_container(parent, mode, &spec, &child);
            if (status != SAO_STATUS_OK)
                return status;
            status = append_nodes(content, child, node["children"]);
            if (status != SAO_STATUS_OK)
                return status;
            continue;
        }

        auto owned = std::make_unique<OwnedWidget>();
        owned->kind = widget_kind(type);
        owned->id = node.value("id", std::string());
        owned->action = node.value("action", std::string());
        owned->enabled = !node.value("disabled", false);
        if (node.contains("payload"))
            owned->action_args = node["payload"].dump();
        owned->props = make_widget_props(node, type);
        if (!owned->id.empty() && content.by_id.contains(owned->id)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        sao_status_t status = sao_ui_widget_create(owned->kind, nullptr, &owned->handle);
        if (status != SAO_STATUS_OK)
            return status;
        const std::string props = owned->props.dump();
        status = sao_ui_widget_apply_props(
            owned->handle, reinterpret_cast<const uint8_t*>(props.data()), props.size());
        if (status != SAO_STATUS_OK)
            return status;

        SaoUiLayoutSpec spec{};
        sao_ui_layout_spec_defaults(&spec);
        if (type == "spacer") {
            spec.fixed_height_px = std::max(0, node.value("size", 8));
        } else if (node.contains("height") && node["height"].is_number_integer()) {
            spec.fixed_height_px = std::max(1, node["height"].get<int32_t>());
        }
        if (node.contains("width") && node["width"].is_number_integer()) {
            spec.fixed_width_px = std::max(1, node["width"].get<int32_t>());
        }
        status = sao_ui_layout_node_add_widget(parent, owned->handle, &spec, &owned->node);
        if (status != SAO_STATUS_OK)
            return status;
        content.by_handle.emplace(owned->handle, owned.get());
        if (!owned->id.empty())
            content.by_id.emplace(owned->id, owned.get());
        content.widgets.push_back(std::move(owned));
    }
    return SAO_STATUS_OK;
}

sao_status_t arrange_content(PanelContent& content, int32_t width, int32_t height, int32_t top) {
    content.root_spec.fixed_width_px = width;
    content.root_spec.fixed_height_px = std::max(1, height - top);
    sao_status_t status = sao_ui_layout_node_set_spec(content.root, &content.root_spec);
    if (status != SAO_STATUS_OK)
        return status;
    SaoUiSize preferred{};
    status = sao_ui_layout_measure(content.root, {width, std::max(1, height - top)}, &preferred);
    if (status != SAO_STATUS_OK)
        return status;
    return sao_ui_layout_arrange(content.root, {0, top, width, std::max(1, height - top)});
}

sao_status_t build_content(const json& normalized, int32_t width, int32_t height, int32_t top,
                           std::shared_ptr<PanelContent>* out_content) {
    if (!normalized.is_object() || !normalized.contains("nodes") ||
        !normalized["nodes"].is_array() || out_content == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto content = std::make_shared<PanelContent>();
    sao_ui_layout_spec_defaults(&content->root_spec);
    content->root_spec.gap_px = 2;
    sao_status_t status = sao_ui_layout_tree_create(&content->tree);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layout_tree_set_root(content->tree, SAO_UI_LAYOUT_VERTICAL, &content->root_spec,
                                         &content->root);
    if (status != SAO_STATUS_OK)
        return status;
    status = append_nodes(*content, content->root, normalized["nodes"]);
    if (status != SAO_STATUS_OK)
        return status;
    status = arrange_content(*content, width, height, top);
    if (status != SAO_STATUS_OK)
        return status;
    *out_content = std::move(content);
    return SAO_STATUS_OK;
}

sao_status_t build_body_content(const SaoUiPanelBodyModelNode* nodes, size_t node_count,
                                int32_t width, int32_t height, int32_t top,
                                std::shared_ptr<PanelContent>* out_content,
                                std::vector<sao_ui_layout_node_handle_t>* out_nodes) {
    if (nodes == nullptr || node_count == 0 || out_content == nullptr || out_nodes == nullptr ||
        nodes[0].model_id == 0 || nodes[0].parent_model_id != 0 || nodes[0].widget != nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto content = std::make_shared<PanelContent>();
    content->root_spec = nodes[0].spec;
    sao_status_t status = sao_ui_layout_tree_create(&content->tree);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_layout_tree_set_root(content->tree, nodes[0].layout_mode, &content->root_spec,
                                         &content->root);
    if (status != SAO_STATUS_OK)
        return status;

    std::unordered_map<uint64_t, sao_ui_layout_node_handle_t> by_model_id;
    by_model_id.emplace(nodes[0].model_id, content->root);
    out_nodes->assign(node_count, nullptr);
    (*out_nodes)[0] = content->root;
    for (size_t index = 1; index < node_count; ++index) {
        const SaoUiPanelBodyModelNode& model = nodes[index];
        if (model.model_id == 0 || by_model_id.contains(model.model_id))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto parent = by_model_id.find(model.parent_model_id);
        if (parent == by_model_id.end())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        sao_ui_layout_node_handle_t actual = nullptr;
        if (model.widget != nullptr) {
            auto widget = std::make_unique<OwnedWidget>();
            widget->handle = model.widget;
            widget->owns_handle = false;
            status =
                sao_ui_layout_node_add_widget(parent->second, model.widget, &model.spec, &actual);
            if (status != SAO_STATUS_OK)
                return status;
            widget->node = actual;
            content->by_handle.emplace(model.widget, widget.get());
            content->widgets.push_back(std::move(widget));
        } else {
            status = sao_ui_layout_node_add_container(parent->second, model.layout_mode,
                                                      &model.spec, &actual);
            if (status != SAO_STATUS_OK)
                return status;
        }
        by_model_id.emplace(model.model_id, actual);
        (*out_nodes)[index] = actual;
    }
    status = arrange_content(*content, width, height, top);
    if (status != SAO_STATUS_OK)
        return status;
    *out_content = std::move(content);
    return SAO_STATUS_OK;
}

sao_status_t paint_panel(const std::shared_ptr<PanelContent>& content, const SaoPanelState& state,
                         bool show_titlebar, const std::string& title, sao_ui_panel_s* panel,
                         sao_ui_offscreen_raster_handle_t raster) {
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_status_t status = sao_ui_paint_ctx_create_offscreen(raster, &context);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_paint_ctx_begin_frame(context);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F, static_cast<float>(state.width),
                                            static_cast<float>(state.height), 0xe61d2430U);
    }
    if (status == SAO_STATUS_OK && show_titlebar) {
        status = sao_ui_paint_ctx_fill_rect(context, 0.0F, 0.0F, static_cast<float>(state.width),
                                            static_cast<float>(kTitlebarHeight), 0xff293a52U);
        if (status == SAO_STATUS_OK && !title.empty()) {
            status =
                sao_ui_paint_ctx_draw_utf8(context, 6.0F, 6.0F, title.c_str(), 12.0F, 0xffffffffU);
        }
    }
    if (status == SAO_STATUS_OK && content != nullptr) {
        std::scoped_lock lock(content->mutex);
        for (const auto& widget : content->widgets) {
            SaoUiRect rect{};
            status = sao_ui_layout_node_get_rect(widget->node, &rect);
            if (status != SAO_STATUS_OK)
                break;
            status = sao_ui_widget_paint_at(widget->handle, context, rect.x_px, rect.y_px,
                                            std::max(1, rect.width_px), std::max(1, rect.height_px),
                                            1.0F);
            if (status != SAO_STATUS_OK)
                break;
        }
    }
    PanelCallbackLease render_lease(panel, CallbackKind::Render);
    if (status == SAO_STATUS_OK && render_lease && render_lease.render() != nullptr) {
        try {
            render_lease.render()(context, 0.0F, 0.0F, static_cast<float>(state.width),
                                  static_cast<float>(state.height), render_lease.user_data());
        } catch (...) {
            status = SAO_STATUS_ERR_UNKNOWN;
        }
    }
    const sao_status_t end_status = sao_ui_paint_ctx_end_frame(context);
    sao_ui_paint_ctx_destroy(context);
    return status == SAO_STATUS_OK ? end_status : status;
}

sao_status_t render_frame(const std::shared_ptr<PanelContent>& content, const SaoPanelState& state,
                          bool show_titlebar, const std::string& title, sao_ui_panel_s* panel,
                          PanelFrame* out_frame) {
    SaoUiOffscreenRasterDesc desc{};
    desc.width_px = static_cast<uint32_t>(state.width);
    desc.height_px = static_cast<uint32_t>(state.height);
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_status_t status = sao_ui_offscreen_raster_create(&desc, &raster);
    if (status != SAO_STATUS_OK)
        return status;
    status = paint_panel(content, state, show_titlebar, title, panel, raster);
    if (status == SAO_STATUS_OK) {
        size_t required = 0;
        status = sao_ui_offscreen_raster_snapshot(raster, nullptr, 0, &required, &out_frame->width,
                                                  &out_frame->height, &out_frame->stride);
        if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
            out_frame->pixels.resize(required);
            status = sao_ui_offscreen_raster_snapshot(
                raster, out_frame->pixels.data(), out_frame->pixels.size(), &required,
                &out_frame->width, &out_frame->height, &out_frame->stride);
        }
    }
    sao_ui_offscreen_raster_destroy(raster);
    return status;
}

sao_status_t snapshot_panel(sao_ui_panel_s* panel, PanelFrame* out_frame) {
    SaoPanelState state{};
    bool titlebar = false;
    std::string title;
    std::shared_ptr<PanelContent> content;
    {
        std::scoped_lock lock(panel->mutex);
        state = panel->state;
        titlebar = panel->show_titlebar;
        title = panel->title;
        content = panel->content;
    }
    return render_frame(content, state, titlebar, title, panel, out_frame);
}

sao_status_t upload_panel(sao_ui_panel_s* panel) {
    PanelRenderGuard render_guard(panel);
    if (!render_guard)
        return SAO_UI_PANEL_STATUS_ERR_BUSY;
    if (panel->layer == nullptr)
        return SAO_STATUS_OK;
    PanelFrame frame;
    const sao_status_t status = snapshot_panel(panel, &frame);
    if (status != SAO_STATUS_OK)
        return status;
    return sao_ui_layer_update_bgra(panel->layer, frame.pixels.data(), frame.width, frame.height,
                                    frame.stride);
}

sao_status_t resolve_action_at(sao_ui_panel_s* panel, int32_t x, int32_t y, std::string* out_action,
                               std::string* out_args) {
    std::shared_ptr<PanelContent> content;
    {
        std::scoped_lock lock(panel->mutex);
        content = panel->content;
    }
    if (content == nullptr)
        return SAO_STATUS_OK;
    {
        std::scoped_lock lock(content->mutex);
        SaoUiHitResult hit{};
        const sao_status_t status = sao_ui_layout_hit_test(content->root, x, y, &hit);
        if (status != SAO_STATUS_OK || hit.widget == nullptr)
            return status;
        const auto found = content->by_handle.find(hit.widget);
        if (found == content->by_handle.end() || !found->second->enabled ||
            found->second->action.empty()) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        *out_action = found->second->action;
        *out_args = found->second->action_args;
    }
    return SAO_STATUS_OK;
}

sao_status_t dispatch_action_at(sao_ui_panel_s* panel, int32_t x, int32_t y) {
    std::string action;
    std::string args;
    const sao_status_t status = resolve_action_at(panel, x, y, &action, &args);
    if (status != SAO_STATUS_OK)
        return status;
    PanelCallbackLease callback(panel, CallbackKind::Action);
    if (!callback || callback.action() == nullptr)
        return SAO_STATUS_OK;
    try {
        callback.action()(action.c_str(), reinterpret_cast<const uint8_t*>(args.data()),
                          args.size(), callback.user_data());
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

void SAO_UI_CALL layer_button_callback(int32_t button, int32_t action, int32_t, float x, float y,
                                       void* user_data) {
    if (button != 0 || action != 1 || user_data == nullptr)
        return;
    auto* panel = static_cast<sao_ui_panel_s*>(user_data);
    PanelOperation operation(panel);
    if (!operation)
        return;
    std::string action_key;
    std::string action_args;
    const sao_status_t status = resolve_action_at(
        panel, static_cast<int32_t>(x), static_cast<int32_t>(y), &action_key, &action_args);
    PanelCallbackLease callback(panel, CallbackKind::Action);
    if (status == SAO_STATUS_OK && callback && callback.action() != nullptr) {
        try {
            callback.action()(action_key.c_str(),
                              reinterpret_cast<const uint8_t*>(action_args.data()),
                              action_args.size(), callback.user_data());
        } catch (...) {
        }
    }
}

void notify(sao_ui_panel_s* panel, int32_t event) {
    PanelCallbackLease callback(panel, CallbackKind::Event);
    if (callback && callback.event() != nullptr) {
        try {
            callback.event()(event, callback.user_data());
        } catch (...) {
        }
    }
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_create(sao_ui_compositor_handle_t compositor,
                                                        const SaoPanelConfig* config,
                                                        sao_ui_panel_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr || config->panel_id_utf8 == nullptr || config->panel_id_utf8[0] == '\0' ||
        config->default_width <= 0 || config->default_height <= 0 || config->min_width < 0 ||
        config->min_height < 0 || config->max_width < 0 || config->max_height < 0 ||
        (config->max_width > 0 && config->max_width < config->min_width) ||
        (config->max_height > 0 && config->max_height < config->min_height))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
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
        auto panel = std::make_unique<sao_ui_panel_s>();
        panel->compositor = compositor;
        panel->id = config->panel_id_utf8;
        panel->title = config->title_utf8 == nullptr ? "" : config->title_utf8;
        panel->show_titlebar = config->show_titlebar;
        panel->min_width = config->min_width;
        panel->min_height = config->min_height;
        panel->max_width = config->max_width;
        panel->max_height = config->max_height;
        panel->state.x = config->default_x;
        panel->state.y = config->default_y;
        panel->state.width =
            clamp_dimension(config->default_width, config->min_width, config->max_width);
        panel->state.height =
            clamp_dimension(config->default_height, config->min_height, config->max_height);
        const json empty_spec{
            {"version", SAO_UI_SPEC_VERSION}, {"title", ""}, {"nodes", json::array()}};
        sao_status_t status = build_content(empty_spec, panel->state.width, panel->state.height,
                                            content_top(*panel), &panel->content);
        if (status != SAO_STATUS_OK)
            return status;

        if (compositor != nullptr) {
            const std::string layer_name = "panel." + panel->id;
            SaoLayerConfig layer_config{};
            layer_config.name_utf8 = layer_name.c_str();
            layer_config.x = panel->state.x;
            layer_config.y = panel->state.y;
            layer_config.width = panel->state.width;
            layer_config.height = panel->state.height;
            layer_config.click_through = false;
            layer_config.rect_hit = true;
            layer_config.bgra_swizzle = true;
            status = sao_ui_layer_create(compositor, &layer_config, &panel->layer);
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_input_callbacks(
                    panel->layer, nullptr, nullptr, &layer_button_callback, nullptr, panel.get());
            }
            if (status == SAO_STATUS_OK)
                status = upload_panel(panel.get());
            if (status == SAO_STATUS_OK) {
                status = sao_ui_layer_set_visible(panel->layer, false);
            }
            if (status != SAO_STATUS_OK)
                return status;
        }
        sao_ui_panel_s* const raw = panel.get();
        panels().push_back(raw);
        try {
            panel_storage().push_back(std::move(panel));
        } catch (...) {
            panels().pop_back();
            throw;
        }
        *out_handle = raw;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_panel_destroy(sao_ui_panel_handle_t panel) {
    if (panel == nullptr)
        return;
    try {
        {
            std::lock_guard registry_lock(panels_mutex());
            auto found = std::ranges::find(panels(), panel);
            if (found == panels().end())
                return;
            panels().erase(found);
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            panel->accepting_operations = false;
            panel->destroy_requested = true;
        }
        {
            std::lock_guard lock(panel->mutex);
            panel->stopping = true;
        }
        finalize_if_ready(panel);
        bool self_callback = false;
        for (const ActiveCallback* active = active_callback; active != nullptr;
             active = active->previous) {
            if (active->panel == panel) {
                self_callback = true;
                break;
            }
        }
        if (self_callback)
            return;
        std::unique_lock lock(panel->lifecycle_mutex);
        panel->lifecycle_cv.wait(lock, [panel] { return panel->finalized; });
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_spec(sao_ui_panel_handle_t panel,
                                                          const uint8_t* spec_json_utf8,
                                                          size_t spec_len) {
    if (panel == nullptr || (spec_json_utf8 == nullptr && spec_len != 0U)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        json normalized;
        std::string serialized;
        sao_status_t status = normalize_spec(spec_json_utf8, spec_len, &normalized, &serialized);
        if (status != SAO_STATUS_OK)
            return status;
        SaoPanelState state{};
        int32_t top = 0;
        {
            std::scoped_lock lock(panel->mutex);
            state = panel->state;
            top = content_top(*panel);
        }
        std::shared_ptr<PanelContent> replacement;
        status = build_content(normalized, state.width, state.height, top, &replacement);
        if (status != SAO_STATUS_OK)
            return status;
        std::shared_ptr<PanelContent> previous;
        std::string previous_spec;
        {
            std::scoped_lock lock(panel->mutex);
            previous = panel->content;
            previous_spec = panel->spec_json;
            panel->content = replacement;
            panel->spec_json = serialized;
        }
        status = upload_panel(panel);
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(panel->mutex);
            panel->content = std::move(previous);
            panel->spec_json = std::move(previous_spec);
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_update_widget(sao_ui_panel_handle_t panel,
                                                               const char* widget_id_utf8,
                                                               const uint8_t* props_json_utf8,
                                                               size_t props_len) {
    if (panel == nullptr || widget_id_utf8 == nullptr || widget_id_utf8[0] == '\0' ||
        (props_json_utf8 == nullptr && props_len != 0U)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        const json patch = props_len == 0
                               ? json::object()
                               : json::parse(props_json_utf8, props_json_utf8 + props_len);
        if (!patch.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::shared_ptr<PanelContent> content;
        {
            std::scoped_lock lock(panel->mutex);
            content = panel->content;
        }
        OwnedWidget* widget = nullptr;
        json previous_props;
        std::string previous_action;
        std::string previous_args;
        bool previous_enabled = true;
        {
            std::scoped_lock lock(content->mutex);
            const auto found = content->by_id.find(widget_id_utf8);
            if (found == content->by_id.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            widget = found->second;
            previous_props = widget->props;
            previous_action = widget->action;
            previous_args = widget->action_args;
            previous_enabled = widget->enabled;
            for (auto it = patch.begin(); it != patch.end(); ++it) {
                widget->props[it.key()] = it.value();
            }
            if (widget->kind == SAO_UI_WIDGET_ACTION_BUTTON && patch.contains("label") &&
                patch["label"].is_string()) {
                widget->props["text"] = patch["label"];
            }
            if (widget->kind == SAO_UI_WIDGET_BAR && patch.contains("pct") &&
                patch["pct"].is_number()) {
                widget->props["value"] = patch["pct"];
            }
            if (patch.contains("action") && patch["action"].is_string()) {
                widget->action = patch["action"].get<std::string>();
            }
            if (patch.contains("payload"))
                widget->action_args = patch["payload"].dump();
            if (patch.contains("disabled")) {
                widget->enabled = !patch.value("disabled", false);
                widget->props["enabled"] = widget->enabled;
            }
            const std::string merged = widget->props.dump();
            const sao_status_t status = sao_ui_widget_apply_props(
                widget->handle, reinterpret_cast<const uint8_t*>(merged.data()), merged.size());
            if (status != SAO_STATUS_OK) {
                widget->props = std::move(previous_props);
                widget->action = std::move(previous_action);
                widget->action_args = std::move(previous_args);
                widget->enabled = previous_enabled;
                return status;
            }
            (void)sao_ui_layout_node_invalidate(widget->node);
        }
        const sao_status_t status = upload_panel(panel);
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(content->mutex);
            widget->props = std::move(previous_props);
            widget->action = std::move(previous_action);
            widget->action_args = std::move(previous_args);
            widget->enabled = previous_enabled;
            const std::string rollback = widget->props.dump();
            (void)sao_ui_widget_apply_props(
                widget->handle, reinterpret_cast<const uint8_t*>(rollback.data()), rollback.size());
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_replace_body_model(
    sao_ui_panel_handle_t panel, const SaoUiPanelBodyModelNode* nodes, size_t node_count,
    sao_ui_layout_node_handle_t* out_layout_nodes, size_t out_layout_node_capacity,
    sao_ui_layout_tree_handle_t* out_tree) {
    if (out_tree != nullptr)
        *out_tree = nullptr;
    if (panel == nullptr || nodes == nullptr || node_count == 0 ||
        (out_layout_nodes != nullptr && out_layout_node_capacity < node_count)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        PanelRenderGuard render_guard(panel);
        if (!render_guard)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;

        SaoPanelState state{};
        bool show_titlebar = false;
        std::string title;
        sao_ui_layer_handle_t layer = nullptr;
        {
            std::lock_guard lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            title = panel->title;
            layer = panel->layer;
        }

        std::shared_ptr<PanelContent> replacement;
        std::vector<sao_ui_layout_node_handle_t> actual_nodes;
        sao_status_t status =
            build_body_content(nodes, node_count, state.width, state.height,
                               show_titlebar ? kTitlebarHeight : 0, &replacement, &actual_nodes);
        if (status != SAO_STATUS_OK)
            return status;

        std::vector<size_t> applied_props;
        applied_props.reserve(node_count);
        struct PropsRollback {
            const SaoUiPanelBodyModelNode* nodes{};
            const std::vector<size_t>* applied{};
            bool armed{true};

            sao_status_t run() {
                if (!armed)
                    return SAO_STATUS_OK;
                armed = false;
                sao_status_t first_failure = SAO_STATUS_OK;
                for (auto it = applied->rbegin(); it != applied->rend(); ++it) {
                    const SaoUiPanelBodyModelNode& node = nodes[*it];
                    sao_status_t rollback_status = SAO_STATUS_OK;
                    if (g_body_replace_failure_point.load(std::memory_order_acquire) == 2) {
                        rollback_status = SAO_STATUS_ERR_UNKNOWN;
                    } else {
                        rollback_status = sao_ui_widget_apply_props(
                            node.widget, node.rollback_props_json_utf8,
                            node.rollback_props_len);
                    }
                    if (first_failure == SAO_STATUS_OK && rollback_status != SAO_STATUS_OK)
                        first_failure = rollback_status;
                }
                return first_failure;
            }

            void dismiss() noexcept {
                armed = false;
            }
        } rollback{nodes, &applied_props};
        const auto fail_with_rollback = [&](sao_status_t failure) {
            return rollback.run() == SAO_STATUS_OK ? failure
                                                   : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
        };
        for (size_t index = 0; index < node_count; ++index) {
            const SaoUiPanelBodyModelNode& node = nodes[index];
            if (node.widget == nullptr || node.props_json_utf8 == nullptr)
                continue;
            status = sao_ui_widget_apply_props(node.widget, node.props_json_utf8, node.props_len);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
            applied_props.push_back(index);
        }

        if (g_body_replace_failure_point.load(std::memory_order_acquire) != 0)
            return fail_with_rollback(SAO_STATUS_ERR_UNKNOWN);

        PanelFrame frame;
        status = render_frame(replacement, state, show_titlebar, title, panel, &frame);
        if (status != SAO_STATUS_OK)
            return fail_with_rollback(status);
        if (layer != nullptr) {
            status = sao_ui_layer_update_bgra(layer, frame.pixels.data(), frame.width, frame.height,
                                              frame.stride);
            if (status != SAO_STATUS_OK)
                return fail_with_rollback(status);
        }
        const sao_ui_layout_tree_handle_t committed_tree = replacement->tree;
        {
            std::lock_guard lock(panel->mutex);
            panel->content = std::move(replacement);
            panel->spec_json.clear();
        }
        rollback.dismiss();
        if (out_tree != nullptr)
            *out_tree = committed_tree;
        if (out_layout_nodes != nullptr) {
            std::copy(actual_nodes.begin(), actual_nodes.end(), out_layout_nodes);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_body_replace_failure_point(int32_t point) {
    g_body_replace_failure_point.store(point, std::memory_order_release);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_visible(sao_ui_panel_handle_t panel,
                                                             bool visible) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        bool changed = false;
        {
            std::scoped_lock lock(panel->mutex);
            changed = panel->state.visible != visible;
        }
        if (panel->layer != nullptr) {
            sao_status_t status = sao_ui_layer_set_visible(panel->layer, visible);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_input_enabled(panel->layer, visible);
            if (status != SAO_STATUS_OK) {
                (void)sao_ui_layer_set_visible(panel->layer, !visible);
                return status;
            }
        }
        {
            std::scoped_lock lock(panel->mutex);
            panel->state.visible = visible;
        }
        if (changed)
            notify(panel, visible ? SAO_UI_PANEL_EVENT_SHOW : SAO_UI_PANEL_EVENT_HIDE);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_position(sao_ui_panel_handle_t panel,
                                                              int32_t x, int32_t y) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        if (panel->layer != nullptr) {
            const sao_status_t status = sao_ui_layer_set_position(panel->layer, x, y);
            if (status != SAO_STATUS_OK)
                return status;
        }
        {
            std::scoped_lock lock(panel->mutex);
            panel->state.x = x;
            panel->state.y = y;
        }
        notify(panel, SAO_UI_PANEL_EVENT_MOVE);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry(sao_ui_panel_handle_t panel,
                                                              int32_t x, int32_t y, int32_t width,
                                                              int32_t height) {
    if (panel == nullptr || width <= 0 || height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        width = clamp_dimension(width, panel->min_width, panel->max_width);
        height = clamp_dimension(height, panel->min_height, panel->max_height);
        SaoPanelState previous{};
        std::shared_ptr<PanelContent> content;
        {
            std::scoped_lock lock(panel->mutex);
            previous = panel->state;
            panel->state.x = x;
            panel->state.y = y;
            panel->state.width = width;
            panel->state.height = height;
            content = panel->content;
        }
        if (content != nullptr) {
            std::scoped_lock lock(content->mutex);
            const sao_status_t arrange_status =
                arrange_content(*content, width, height, content_top(*panel));
            if (arrange_status != SAO_STATUS_OK) {
                std::scoped_lock panel_lock(panel->mutex);
                panel->state = previous;
                (void)arrange_content(*content, previous.width, previous.height,
                                      content_top(*panel));
                return arrange_status;
            }
        }
        sao_status_t status = upload_panel(panel);
        if (status == SAO_STATUS_OK && panel->layer != nullptr)
            status = sao_ui_layer_set_position(panel->layer, x, y);
        if (status != SAO_STATUS_OK) {
            {
                std::scoped_lock lock(panel->mutex);
                panel->state = previous;
            }
            if (content != nullptr) {
                std::scoped_lock lock(content->mutex);
                (void)arrange_content(*content, previous.width, previous.height,
                                      content_top(*panel));
            }
            (void)upload_panel(panel);
            return status;
        }
        notify(panel, SAO_UI_PANEL_EVENT_RESIZE);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_ui_layer_handle_t SAO_UI_CALL sao_ui_panel_layer(sao_ui_panel_handle_t panel) {
    PanelOperation operation(panel);
    if (!operation)
        return nullptr;
    try {
        std::lock_guard lock(panel->mutex);
        return panel->layer;
    } catch (...) {
        return nullptr;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_get_layout_tree(sao_ui_panel_handle_t panel, sao_ui_layout_tree_handle_t* out_tree,
                             sao_ui_layout_node_handle_t* out_root) {
    if (panel == nullptr || out_tree == nullptr || out_root == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::scoped_lock lock(panel->mutex);
        if (panel->content == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        *out_tree = panel->content->tree;
        *out_root = panel->content->root;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_rasterize(sao_ui_panel_handle_t panel, sao_ui_offscreen_raster_handle_t raster) {
    if (panel == nullptr || raster == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        PanelRenderGuard render_guard(panel);
        if (!render_guard)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        SaoPanelState state{};
        bool show_titlebar = false;
        std::string title;
        std::shared_ptr<PanelContent> content;
        {
            std::scoped_lock lock(panel->mutex);
            state = panel->state;
            show_titlebar = panel->show_titlebar;
            title = panel->title;
            content = panel->content;
        }
        return paint_panel(content, state, show_titlebar, title, panel, raster);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_action_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_action_callback_t callback, void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Action)]++;
            std::lock_guard state_lock(panel->mutex);
            panel->action_callback = callback;
            panel->action_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Action, previous_generation);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_event_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_event_callback_t callback, void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Event)]++;
            std::lock_guard state_lock(panel->mutex);
            panel->event_callback = callback;
            panel->event_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Event, previous_generation);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_render_fn(sao_ui_panel_handle_t panel,
                                                               sao_ui_panel_render_fn_t callback,
                                                               void* user_data) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard render_lock(panel->render_mutex);
        sao_ui_panel_render_fn_t previous_callback = nullptr;
        void* previous_user_data = nullptr;
        uint64_t previous_generation = 0;
        {
            std::lock_guard lifecycle_lock(panel->lifecycle_mutex);
            previous_generation =
                panel->callback_generations[static_cast<size_t>(CallbackKind::Render)]++;
            std::lock_guard lock(panel->mutex);
            previous_callback = panel->render_callback;
            previous_user_data = panel->render_user_data;
            panel->render_callback = callback;
            panel->render_user_data = user_data;
        }
        wait_for_callback_generation(panel, CallbackKind::Render, previous_generation);
        const sao_status_t status = upload_panel(panel);
        if (status != SAO_STATUS_OK) {
            std::scoped_lock lock(panel->mutex);
            panel->render_callback = previous_callback;
            panel->render_user_data = previous_user_data;
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_get_state(sao_ui_panel_handle_t panel,
                                                           SaoPanelState* out_state) {
    if (panel == nullptr || out_state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::scoped_lock lock(panel->mutex);
        *out_state = panel->state;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_find_by_id(sao_ui_compositor_handle_t compositor,
                                                            const char* panel_id_utf8,
                                                            sao_ui_panel_handle_t* out_handle) {
    if (out_handle == nullptr || panel_id_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> registry_lock(panels_mutex());
        for (sao_ui_panel_s* panel : panels()) {
            if (panel->compositor == compositor && panel->id == panel_id_utf8) {
                *out_handle = panel;
                return SAO_STATUS_OK;
            }
        }
        *out_handle = nullptr;
        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        *out_handle = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_enumerate(sao_ui_compositor_handle_t compositor,
                                                           sao_ui_panel_handle_t* out_handles,
                                                           size_t capacity, size_t* out_written) {
    if (out_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> registry_lock(panels_mutex());
        size_t total = 0;
        for (sao_ui_panel_s* panel : panels()) {
            if (panel->compositor != compositor)
                continue;
            if (out_handles != nullptr && total < capacity)
                out_handles[total] = panel;
            ++total;
        }
        *out_written = total;
        return out_handles != nullptr && capacity < total ? SAO_STATUS_ERR_BUFFER_TOO_SMALL
                                                          : SAO_STATUS_OK;
    } catch (...) {
        *out_written = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_test_pointer_button(sao_ui_panel_handle_t panel, int32_t x, int32_t y) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    PanelOperation operation(panel);
    if (!operation)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        return dispatch_action_at(panel, x, y);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
