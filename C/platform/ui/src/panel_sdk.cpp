// SAO Auto — panel SDK descriptor and body-mutation path.
//
// Modern registration path for plugin panels.  Plugins fill a
// SaoPanelDescriptor, call sao_ui_panel_register, then interact with
// the returned panel/body handles via panel_layout.h + this file's
// batched body-mutation API.
//
// The SDK registry owns a classic runtime panel, so body replacement,
// layer state, actions, and geometry events use the same production
// path.  Headless tests use a software compositor rather than a
// synthetic registry-only result.
//
// Python source alignment (memory `别造额外UI入口`,
// `面板组件库支持颜色覆盖`, `ACT扁平化机制`):
//   * plugin_manager.register_panel     — parent of this file
//   * gui_modules/sao_panel_ui.py       — descriptor semantics
//   * sao_theme/panel_themes            — theme_override_json_utf8
//   * gpu_overlay/z_order.py            — z_class + z_within_class

#include "sao/ui/panel_sdk.h"
#include "sao/ui/theme.h"
#include "sao/ui/widget_kit.h"

#include "widget_typed_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_apply_theme_override_(
    sao_ui_panel_handle_t panel, const uint8_t* override_json_utf8, size_t override_len);

std::atomic<int32_t> g_panel_publish_failure_point{0};
std::atomic_size_t g_geometry_worker_count{0};

void fail_panel_publish_at(int32_t point) {
    if (g_panel_publish_failure_point.load(std::memory_order_acquire) == point)
        throw std::bad_alloc{};
}

// ─── Internal panel record ──────────────────────────────────────────
//
// A registered panel is a POD-ish record kept in a process-wide map
// keyed by descriptor id.  The map serves two roles:
//   1. duplicate-id guard (memory `别造额外UI入口`: one plugin = one
//      main panel, no accidental double-register from show/hide),
//   2. iteration surface for the launcher lifecycle sweep.

struct BodyRecord {
    struct Node {
        uint64_t id{};
        uint64_t parent_id{};
        int32_t sibling_order{};
        int32_t layout_mode{SAO_UI_LAYOUT_VERTICAL};
        SaoUiLayoutSpec spec{};
        sao_ui_widget_handle_t widget{};
        json props{json::object()};
        json committed_props{json::object()};
    };

    sao_ui_layout_tree_handle_t tree = nullptr;
    sao_ui_layout_node_handle_t root = nullptr;
    std::vector<Node> model;
    std::vector<std::pair<sao_ui_layout_node_handle_t, uint64_t>> actual_to_model;
    uint64_t next_node_id{2};
    std::vector<int> mutation_log; // records mutation kinds in order
    uint64_t mutation_count_total = 0;
};

struct BodyHandleShell {
    uint64_t generation{};
};

struct GeometryPersistence {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    std::string panel_id;
    sao_ui_panel_handle_t runtime_panel{};
    sao_ui_panel_geometry_cb_t callback{};
    void* user_data{};
    SaoPanelState pending{};
    uint64_t generation{};
    uint64_t callback_generation{1};
    bool remember{};
    bool has_pending{};
    bool stopping{};
    std::shared_ptr<std::atomic_bool> deferred_completion{
        std::make_shared<std::atomic_bool>(false)};
    std::unordered_map<uint64_t, size_t> callbacks_in_flight;
    std::function<void()> deferred_cleanup;

    struct Active {
        GeometryPersistence* owner{};
        uint64_t generation{};
        Active* previous{};
    };

    static thread_local Active* active;

    ~GeometryPersistence() {
        try {
            stop();
        } catch (...) {
        }
    }

    sao_status_t start() noexcept {
        try {
            worker = std::thread([this] {
                struct WorkerLifetime {
                    WorkerLifetime() {
                        g_geometry_worker_count.fetch_add(1, std::memory_order_acq_rel);
                    }

                    ~WorkerLifetime() {
                        g_geometry_worker_count.fetch_sub(1, std::memory_order_acq_rel);
                    }
                } worker_lifetime;
                std::unique_lock lock(mutex);
                while (!stopping) {
                    cv.wait(lock, [this] { return stopping || has_pending; });
                    if (stopping)
                        break;
                    const uint64_t observed = generation;
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    if (cv.wait_until(lock, deadline, [this, observed] {
                            return stopping || generation != observed;
                        })) {
                        continue;
                    }
                    const SaoPanelState geometry = pending;
                    const auto fn = callback;
                    void* const data = user_data;
                    const uint64_t callback_version = callback_generation;
                    has_pending = false;
                    if (fn != nullptr)
                        ++callbacks_in_flight[callback_version];
                    lock.unlock();
                    if (fn != nullptr) {
                        Active marker{this, callback_version, active};
                        active = &marker;
                        try {
                            fn(panel_id.c_str(), geometry.x, geometry.y, geometry.width,
                               geometry.height, data);
                        } catch (...) {
                        }
                        active = marker.previous;
                    }
                    lock.lock();
                    if (fn != nullptr) {
                        auto found = callbacks_in_flight.find(callback_version);
                        if (found != callbacks_in_flight.end() && --found->second == 0)
                            callbacks_in_flight.erase(found);
                        cv.notify_all();
                    }
                    if (stopping && deferred_cleanup) {
                        auto cleanup = std::move(deferred_cleanup);
                        const auto completion = deferred_completion;
                        lock.unlock();
                        if (worker.joinable())
                            worker.detach();
                        cleanup();
                        completion->store(true, std::memory_order_release);
                        return;
                    }
                }
            });
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        return SAO_STATUS_OK;
    }

    void schedule(const SaoPanelState& geometry) {
        std::lock_guard lock(mutex);
        if (!remember || callback == nullptr || stopping)
            return;
        pending = geometry;
        has_pending = true;
        ++generation;
        cv.notify_all();
    }

    void set_handler(sao_ui_panel_geometry_cb_t fn, void* data) {
        uint64_t previous = 0;
        {
            std::lock_guard lock(mutex);
            if (stopping)
                return;
            previous = callback_generation++;
            callback = fn;
            user_data = data;
            if (fn == nullptr)
                has_pending = false;
            ++generation;
        }
        cv.notify_all();
        if (is_active(previous))
            return;
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return !callbacks_in_flight.contains(previous); });
    }

    bool is_worker_thread() {
        std::lock_guard lock(mutex);
        return worker.joinable() && worker.get_id() == std::this_thread::get_id();
    }

    bool deferred_stop_complete() {
        return deferred_completion->load(std::memory_order_acquire);
    }

    bool is_active(uint64_t callback_version) const noexcept {
        for (const Active* current = active; current != nullptr; current = current->previous) {
            if (current->owner == this && current->generation == callback_version)
                return true;
        }
        return false;
    }

    void defer_stop(std::function<void()> cleanup) {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            has_pending = false;
            deferred_cleanup = std::move(cleanup);
            ++generation;
        }
        cv.notify_all();
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            has_pending = false;
            ++generation;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return callbacks_in_flight.empty(); });
    }
};

thread_local GeometryPersistence::Active* GeometryPersistence::active = nullptr;

struct PanelRecord {
    std::recursive_mutex mutex;
    bool active{true};
    // Persistent copy of the descriptor.  All const char* fields are
    // deep-copied into the strings below so plugins may free their
    // input immediately after register().
    std::string panel_id;
    std::string title;
    std::string follow_panel_id;
    std::string theme_override_json;
    std::vector<uint8_t> icon_pixels;

    // Descriptor cache (rebuilt on read so we never hand out dangling
    // pointers).  Kept as raw copy of the numeric fields.
    SaoPanelDescriptor descriptor_cache{};

    // Compositor + geometry state.
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_panel_handle_t runtime_panel = nullptr;

    // Runtime state.
    bool visible = true;
    float opacity_0_to_1 = 1.0f;
    int32_t z_class = 0;
    int32_t z_within_class = 0;

    // Body sub-record.
    std::unique_ptr<BodyRecord> body;
    sao_ui_panel_body_handle_t body_handle{};
    uint64_t body_generation{};
    GeometryPersistence geometry_persistence;

    // Handle bookkeeping.
    uint64_t id_num = 0; // monotonic id assigned at register-time
};

class PanelRegistrationGuard {
  public:
    explicit PanelRegistrationGuard(std::shared_ptr<PanelRecord> record)
        : record_(std::move(record)) {}

    PanelRegistrationGuard(const PanelRegistrationGuard&) = delete;
    PanelRegistrationGuard& operator=(const PanelRegistrationGuard&) = delete;

    ~PanelRegistrationGuard() {
        cleanup();
    }

    void worker_started() noexcept {
        worker_started_ = true;
    }

    void release() noexcept {
        armed_ = false;
    }

  private:
    void cleanup() noexcept {
        if (!armed_ || record_ == nullptr || record_->runtime_panel == nullptr)
            return;
        try {
            (void)sao_ui_panel_set_event_handler(record_->runtime_panel, nullptr, nullptr);
        } catch (...) {
        }
        if (worker_started_) {
            try {
                record_->geometry_persistence.stop();
            } catch (...) {
            }
        }
        try {
            sao_ui_panel_destroy(record_->runtime_panel);
        } catch (...) {
        }
        record_->runtime_panel = nullptr;
        record_->geometry_persistence.runtime_panel = nullptr;
    }

    std::shared_ptr<PanelRecord> record_;
    bool worker_started_{};
    bool armed_{true};
};

// ─── Global registry ────────────────────────────────────────────────
struct Registry {
    std::mutex mu;
    // Keyed by the opaque panel handle we hand out.  We use a raw
    // pointer-as-integer scheme: the handle IS the record pointer,
    // so lookup is O(1) via reinterpret_cast (bounded by an alive-set
    // guard below).
    std::unordered_map<sao_ui_panel_handle_t, std::shared_ptr<PanelRecord>> panels;
    // Body shell → panel back-pointer. Shell storage is permanent so an old
    // opaque address can never alias a later panel body.
    std::unordered_map<sao_ui_panel_body_handle_t, std::weak_ptr<PanelRecord>> body_index;
    std::vector<std::unique_ptr<BodyHandleShell>> body_shells;
    // panel_id → panel*, for duplicate-id detection.
    std::unordered_map<std::string, sao_ui_panel_handle_t> by_id;
    std::vector<std::shared_ptr<PanelRecord>> retired;
    uint64_t next_id = 1;
    uint64_t next_body_generation = 1;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

std::mutex& z_order_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<PanelRecord> lookup(sao_ui_panel_handle_t handle) {
    if (handle == nullptr)
        return {};
    auto& reg = registry();
    auto it = reg.panels.find(handle);
    return (it == reg.panels.end()) ? std::shared_ptr<PanelRecord>() : it->second;
}

std::shared_ptr<PanelRecord> lookup_body_owner(sao_ui_panel_body_handle_t handle) {
    if (handle == nullptr)
        return {};
    auto& reg = registry();
    const auto it = reg.body_index.find(handle);
    if (it == reg.body_index.end())
        return {};
    const auto owner = it->second.lock();
    if (owner == nullptr || owner->body_handle != handle)
        return {};
    const auto* shell = reinterpret_cast<const BodyHandleShell*>(handle);
    return shell->generation == owner->body_generation ? owner : std::shared_ptr<PanelRecord>();
}

void reap_retired_panels() {
    auto& reg = registry();
    std::lock_guard lock(reg.mu);
    std::erase_if(reg.retired, [](const std::shared_ptr<PanelRecord>& record) {
        return record->geometry_persistence.deferred_stop_complete();
    });
}

std::shared_ptr<PanelRecord> registered_panel(sao_ui_panel_handle_t handle) {
    auto& reg = registry();
    std::lock_guard lock(reg.mu);
    return lookup(handle);
}

std::shared_ptr<PanelRecord> registered_body_owner(sao_ui_panel_body_handle_t handle) {
    auto& reg = registry();
    std::lock_guard lock(reg.mu);
    return lookup_body_owner(handle);
}

void SAO_UI_CALL runtime_panel_event(int32_t event_kind, void* user_data) {
    if (event_kind != SAO_UI_PANEL_EVENT_MOVE && event_kind != SAO_UI_PANEL_EVENT_RESIZE)
        return;
    auto* persistence = static_cast<GeometryPersistence*>(user_data);
    if (persistence == nullptr)
        return;
    SaoPanelState state{};
    if (sao_ui_panel_get_state(persistence->runtime_panel, &state) == SAO_STATUS_OK) {
        persistence->schedule(state);
    }
}

void rebuild_descriptor_cache(PanelRecord& rec) {
    // Rewire the pointer fields to point into the record's owned
    // strings — this is the shape the caller sees from get_descriptor.
    rec.descriptor_cache.panel_id_utf8 = rec.panel_id.empty() ? nullptr : rec.panel_id.c_str();
    rec.descriptor_cache.title_utf8 = rec.title.empty() ? nullptr : rec.title.c_str();
    rec.descriptor_cache.follow_panel_id_utf8 =
        rec.follow_panel_id.empty() ? nullptr : rec.follow_panel_id.c_str();
    rec.descriptor_cache.theme_override_json_utf8 =
        rec.theme_override_json.empty() ? nullptr : rec.theme_override_json.c_str();
    rec.descriptor_cache.icon_bgra_pixels =
        rec.icon_pixels.empty() ? nullptr : rec.icon_pixels.data();
}

int32_t global_z_key(int32_t z_class, int32_t z_within_class) {
    constexpr int32_t kBandCenter = 1'000'000'000;
    constexpr int32_t kLocalLimit = 250'000'000;
    const int32_t local = std::clamp(z_within_class, -kLocalLimit, kLocalLimit);
    if (z_class == SAO_UI_PANEL_Z_BOTTOM)
        return -kBandCenter + local;
    if (z_class == SAO_UI_PANEL_Z_TOPMOST)
        return kBandCenter + local;
    return local;
}

sao_status_t validate_body_widget(sao_ui_widget_handle_t widget) {
    if (widget == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    int32_t kind = -1;
    return sao_ui_widget_get_kind(widget, &kind);
}

void initialize_body_model(BodyRecord& body) {
    BodyRecord::Node root{};
    root.id = 1;
    root.layout_mode = SAO_UI_LAYOUT_VERTICAL;
    sao_ui_layout_spec_defaults(&root.spec);
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    (void)sao_ui_theme_get_active_id(&theme_id);
    const int32_t padding =
        sao_ui_theme_resolve_metric(theme_id, SAO_UI_METRIC_PADDING_M);
    root.spec.pad_top_px = padding;
    root.spec.pad_right_px = padding;
    root.spec.pad_bottom_px = padding;
    root.spec.pad_left_px = padding;
    root.spec.gap_px = sao_ui_theme_resolve_metric(theme_id, SAO_UI_METRIC_GAP_S);
    body.model.push_back(std::move(root));
    body.actual_to_model.emplace_back(body.root, 1);
}

BodyRecord::Node* model_node(std::vector<BodyRecord::Node>& model, uint64_t id) {
    const auto found =
        std::ranges::find_if(model, [id](const BodyRecord::Node& node) { return node.id == id; });
    return found == model.end() ? nullptr : &*found;
}

const BodyRecord::Node* model_node(const std::vector<BodyRecord::Node>& model, uint64_t id) {
    const auto found =
        std::ranges::find_if(model, [id](const BodyRecord::Node& node) { return node.id == id; });
    return found == model.end() ? nullptr : &*found;
}

int32_t next_sibling_order(const std::vector<BodyRecord::Node>& model, uint64_t parent_id) {
    int32_t next = 0;
    for (const auto& node : model) {
        if (node.parent_id == parent_id)
            next = std::max(next, node.sibling_order + 1);
    }
    return next;
}

void remove_model_subtree(std::vector<BodyRecord::Node>& model, uint64_t root_id) {
    std::vector<uint64_t> removed{root_id};
    for (size_t index = 0; index < removed.size(); ++index) {
        for (const auto& node : model) {
            if (node.parent_id == removed[index])
                removed.push_back(node.id);
        }
    }
    std::erase_if(model, [&](const BodyRecord::Node& node) {
        return std::ranges::find(removed, node.id) != removed.end();
    });
}

sao_status_t reorder_model_node(std::vector<BodyRecord::Node>& model, uint64_t id,
                                int32_t new_index) {
    BodyRecord::Node* target = model_node(model, id);
    if (target == nullptr || target->parent_id == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const uint64_t parent_id = target->parent_id;
    std::vector<BodyRecord::Node*> siblings;
    for (auto& node : model) {
        if (node.parent_id == parent_id)
            siblings.push_back(&node);
    }
    std::ranges::sort(siblings, {}, &BodyRecord::Node::sibling_order);
    const auto found = std::ranges::find(siblings, target);
    if (found == siblings.end())
        return SAO_STATUS_ERR_NOT_FOUND;
    siblings.erase(found);
    const size_t clamped =
        static_cast<size_t>(std::clamp(new_index, 0, static_cast<int32_t>(siblings.size())));
    siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(clamped), target);
    for (size_t index = 0; index < siblings.size(); ++index)
        siblings[index]->sibling_order = static_cast<int32_t>(index);
    return SAO_STATUS_OK;
}

sao_status_t order_model(const std::vector<BodyRecord::Node>& model,
                         std::vector<const BodyRecord::Node*>* out) {
    if (model.empty() || model_node(model, 1) == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out->clear();
    out->reserve(model.size());
    std::function<void(uint64_t)> append_children = [&](uint64_t parent_id) {
        std::vector<const BodyRecord::Node*> children;
        for (const auto& node : model) {
            if (node.parent_id == parent_id)
                children.push_back(&node);
        }
        std::ranges::sort(children, {}, &BodyRecord::Node::sibling_order);
        for (const BodyRecord::Node* child : children) {
            out->push_back(child);
            append_children(child->id);
        }
    };
    out->push_back(model_node(model, 1));
    append_children(1);
    return out->size() == model.size() ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

} // namespace

// ─── Registration ────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_register(sao_ui_compositor_handle_t compositor,
                                                          const SaoPanelDescriptor* descriptor,
                                                          sao_ui_panel_handle_t* out_panel,
                                                          sao_ui_panel_body_handle_t* out_body) {
    if (out_panel != nullptr)
        *out_panel = nullptr;
    if (out_body != nullptr)
        *out_body = nullptr;
    if (descriptor == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // ABI v1 guard (see SAO_UI_PANEL_DESCRIPTOR_V1_SIZE). struct_size == 0
    // keeps legacy callers (compiled before ABI minor 8) working; other
    // values must fit within the platform's compiled struct so callers
    // cannot claim to send fields the platform does not know about.
    {
        const uint32_t declared = descriptor->struct_size == 0u
                                      ? SAO_UI_PANEL_DESCRIPTOR_V1_SIZE
                                      : descriptor->struct_size;
        if (declared < SAO_UI_PANEL_DESCRIPTOR_V1_SIZE ||
            declared > sizeof(SaoPanelDescriptor)) {
            return SAO_STATUS_ERR_ABI_MISMATCH;
        }
    }
    if (descriptor->panel_id_utf8 == nullptr ||
        descriptor->panel_id_utf8[0] == '\0' || !std::isfinite(descriptor->initial_opacity) ||
        (descriptor->z_class != SAO_UI_PANEL_Z_BOTTOM &&
         descriptor->z_class != SAO_UI_PANEL_Z_NORMAL &&
         descriptor->z_class != SAO_UI_PANEL_Z_TOPMOST)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        reap_retired_panels();
        const bool has_icon = descriptor->icon_bgra_pixels != nullptr ||
                              descriptor->icon_width != 0 || descriptor->icon_height != 0 ||
                              descriptor->icon_stride != 0;
        if (has_icon &&
            (descriptor->icon_bgra_pixels == nullptr || descriptor->icon_width == 0 ||
             descriptor->icon_height == 0 || descriptor->icon_width > UINT32_MAX / 4U ||
             descriptor->icon_stride < descriptor->icon_width * 4U ||
             static_cast<size_t>(descriptor->icon_stride) > SIZE_MAX / descriptor->icon_height)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        auto& reg = registry();
        const std::string id_str(descriptor->panel_id_utf8);
        {
            std::lock_guard guard(reg.mu);
            if (reg.by_id.contains(id_str))
                return SAO_STATUS_ERR_ALREADY_EXISTS;
        }

        auto rec = std::make_shared<PanelRecord>();
        PanelRegistrationGuard registration_guard(rec);
        rec->panel_id = id_str;
        rec->title = descriptor->title_utf8 == nullptr ? "" : descriptor->title_utf8;
        rec->follow_panel_id =
            descriptor->follow_panel_id_utf8 == nullptr ? "" : descriptor->follow_panel_id_utf8;
        rec->theme_override_json = descriptor->theme_override_json_utf8 == nullptr
                                       ? ""
                                       : descriptor->theme_override_json_utf8;
        if (has_icon) {
            const auto* pixels = static_cast<const uint8_t*>(descriptor->icon_bgra_pixels);
            const size_t bytes =
                static_cast<size_t>(descriptor->icon_stride) * descriptor->icon_height;
            rec->icon_pixels.assign(pixels, pixels + bytes);
        }
        rec->descriptor_cache = *descriptor;
        // Force our internal record to advertise the platform's compiled
        // size regardless of the caller's declaration.
        rec->descriptor_cache.struct_size = sizeof(SaoPanelDescriptor);
        rec->descriptor_cache._reserved0 = 0u;
        rec->compositor = compositor;
        rec->visible = descriptor->visible;
        rec->opacity_0_to_1 = std::clamp(descriptor->initial_opacity, 0.0F, 1.0F);
        rec->z_class = descriptor->z_class;
        rec->z_within_class = descriptor->z_within_class;
        rec->body = std::make_unique<BodyRecord>();
        rebuild_descriptor_cache(*rec);

        SaoPanelConfig runtime_config{};
        runtime_config.panel_id_utf8 = descriptor->panel_id_utf8;
        runtime_config.title_utf8 = descriptor->title_utf8;
        runtime_config.default_x = descriptor->default_x_px;
        runtime_config.default_y = descriptor->default_y_px;
        runtime_config.default_width = descriptor->default_width_px;
        runtime_config.default_height = descriptor->default_height_px;
        runtime_config.min_width = descriptor->min_width_px;
        runtime_config.min_height = descriptor->min_height_px;
        runtime_config.max_width = descriptor->max_width_px;
        runtime_config.max_height = descriptor->max_height_px;
        runtime_config.resizable = descriptor->resizable;
        runtime_config.movable = descriptor->movable;
        runtime_config.show_titlebar = descriptor->show_titlebar;
        runtime_config.show_close_button = descriptor->show_close_button;
        runtime_config.remember_geometry = descriptor->remember_geometry;
        runtime_config.rendering_mode = SAO_UI_PANEL_RENDER_NATIVE;
        runtime_config.flat_mode = SAO_UI_PANEL_FLAT_INHERIT;

        sao_status_t status = sao_ui_panel_create(compositor, &runtime_config, &rec->runtime_panel);
        if (status != SAO_STATUS_OK)
            return status;
        if (!rec->theme_override_json.empty()) {
            status = sao_ui_panel_apply_theme_override_(
                rec->runtime_panel,
                reinterpret_cast<const uint8_t*>(rec->theme_override_json.data()),
                rec->theme_override_json.size());
        }
        status =
            status == SAO_STATUS_OK
                ? sao_ui_panel_get_layout_tree(rec->runtime_panel, &rec->body->tree,
                                               &rec->body->root)
                : status;
        rec->geometry_persistence.runtime_panel = rec->runtime_panel;
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_set_event_handler(rec->runtime_panel, &runtime_panel_event,
                                                    &rec->geometry_persistence);
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_set_visible(rec->runtime_panel, rec->visible);
        const sao_ui_layer_handle_t layer = sao_ui_panel_layer(rec->runtime_panel);
        if (status == SAO_STATUS_OK && layer != nullptr)
            status =
                sao_ui_layer_set_z_order(layer, global_z_key(rec->z_class, rec->z_within_class));
        if (status == SAO_STATUS_OK && layer != nullptr)
            status = sao_ui_layer_set_alpha(layer, rec->opacity_0_to_1);
        if (status != SAO_STATUS_OK)
            return status;
        initialize_body_model(*rec->body);
        rec->geometry_persistence.panel_id = id_str;
        rec->geometry_persistence.remember = descriptor->remember_geometry;
        status = rec->geometry_persistence.start();
        if (status != SAO_STATUS_OK)
            return status;
        registration_guard.worker_started();

        const sao_ui_panel_handle_t runtime_panel = rec->runtime_panel;
        bool duplicate = false;
        {
            std::lock_guard guard(reg.mu);
            if (reg.by_id.contains(id_str)) {
                duplicate = true;
            } else {
                const size_t shell_count = reg.body_shells.size();
                try {
                    fail_panel_publish_at(1);
                    auto shell = std::make_unique<BodyHandleShell>();
                    shell->generation = reg.next_body_generation++;
                    if (shell->generation == 0)
                        shell->generation = reg.next_body_generation++;
                    rec->body_handle = reinterpret_cast<sao_ui_panel_body_handle_t>(shell.get());
                    rec->body_generation = shell->generation;
                    reg.body_shells.push_back(std::move(shell));
                    fail_panel_publish_at(2);
                    rec->id_num = reg.next_id++;
                    reg.panels.emplace(runtime_panel, rec);
                    fail_panel_publish_at(3);
                    reg.body_index.emplace(rec->body_handle, rec);
                    fail_panel_publish_at(4);
                    reg.by_id.emplace(id_str, runtime_panel);
                    fail_panel_publish_at(5);
                } catch (...) {
                    reg.panels.erase(runtime_panel);
                    reg.body_index.erase(rec->body_handle);
                    reg.by_id.erase(id_str);
                    while (reg.body_shells.size() > shell_count)
                        reg.body_shells.pop_back();
                    rec->body_handle = nullptr;
                    rec->body_generation = 0;
                    throw;
                }
            }
        }
        if (duplicate)
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        registration_guard.release();
        if (out_panel != nullptr)
            *out_panel = runtime_panel;
        if (out_body != nullptr)
            *out_body = rec->body_handle;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_unregister(sao_ui_panel_handle_t panel) {
    if (panel == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto& reg = registry();
        std::shared_ptr<PanelRecord> rec;
        {
            std::lock_guard lock(reg.mu);
            auto it = reg.panels.find(panel);
            if (it == reg.panels.end())
                return SAO_STATUS_ERR_NOT_FOUND;
            rec = it->second;
            reg.by_id.erase(rec->panel_id);
            reg.body_index.erase(rec->body_handle);
            reg.panels.erase(it);
        }
        {
            std::lock_guard lock(rec->mutex);
            rec->active = false;
        }
        (void)sao_ui_panel_set_event_handler(rec->runtime_panel, nullptr, nullptr);
        if (rec->geometry_persistence.is_worker_thread()) {
            {
                std::lock_guard lock(reg.mu);
                reg.retired.push_back(rec);
            }
            rec->geometry_persistence.defer_stop(
                [rec] { sao_ui_panel_destroy(rec->runtime_panel); });
            return SAO_STATUS_OK;
        }
        rec->geometry_persistence.stop();
        sao_ui_panel_destroy(rec->runtime_panel);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Body mutations (batched) ────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_update_body(sao_ui_panel_body_handle_t body,
                                                             const SaoUiBodyMutation* mutations,
                                                             size_t mutation_count) {
    if (body == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (mutation_count != 0 && mutations == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto& reg = registry();
        std::shared_ptr<PanelRecord> owner;
        {
            std::lock_guard registry_lock(reg.mu);
            owner = lookup_body_owner(body);
        }
        if (owner == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;

        std::lock_guard owner_lock(owner->mutex);
        if (!owner->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (mutation_count == 0)
            return SAO_STATUS_OK;
        BodyRecord& current = *owner->body;
        std::vector<BodyRecord::Node> staged = current.model;
        uint64_t next_id = current.next_node_id;
        std::vector<uint64_t> created_by_mutation(mutation_count, 0);
        struct TypedPropsPatch {
            sao_ui_widget_handle_t widget{};
            json props{json::object()};
        };
        std::vector<TypedPropsPatch> typed_props;

        const auto target_id = [&](sao_ui_layout_node_handle_t target) -> uint64_t {
            if (target == nullptr)
                return 1;
            const auto found =
                std::ranges::find_if(current.actual_to_model,
                                     [target](const auto& entry) { return entry.first == target; });
            return found == current.actual_to_model.end() ? 0 : found->second;
        };

        for (size_t index = 0; index < mutation_count; ++index) {
            const SaoUiBodyMutation& mutation = mutations[index];
            const uint64_t target = target_id(mutation.target);
            BodyRecord::Node* target_node = model_node(staged, target);
            switch (mutation.kind) {
            case SAO_UI_BODY_ADD_WIDGET:
            case SAO_UI_BODY_ADD_CONTAINER: {
                const bool widget_node = mutation.kind == SAO_UI_BODY_ADD_WIDGET;
                if (target_node == nullptr || target_node->widget != nullptr ||
                    mutation.spec == nullptr || (widget_node && mutation.widget == nullptr) ||
                    (!widget_node && mutation.widget != nullptr)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                if (widget_node) {
                    const sao_status_t widget_status = validate_body_widget(mutation.widget);
                    if (widget_status != SAO_STATUS_OK)
                        return widget_status;
                }
                BodyRecord::Node added{};
                added.id = next_id++;
                added.parent_id = target;
                added.sibling_order = next_sibling_order(staged, target);
                added.layout_mode = mutation.layout_mode;
                added.spec = *mutation.spec;
                added.widget = mutation.widget;
                staged.push_back(std::move(added));
                created_by_mutation[index] = staged.back().id;
                break;
            }
            case SAO_UI_BODY_REMOVE_NODE:
                if (mutation.target == nullptr || target_node == nullptr || target == 1)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                remove_model_subtree(staged, target);
                break;
            case SAO_UI_BODY_UPDATE_SPEC:
                if (target_node == nullptr || mutation.spec == nullptr)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                target_node->spec = *mutation.spec;
                break;
            case SAO_UI_BODY_REORDER_NODE: {
                if (mutation.target == nullptr || target_node == nullptr)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const sao_status_t status = reorder_model_node(staged, target, mutation.new_index);
                if (status != SAO_STATUS_OK)
                    return status;
                break;
            }
            case SAO_UI_BODY_UPDATE_WIDGET_PROPS: {
                if (mutation.widget == nullptr)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                if (mutation.props_json_utf8 == nullptr && mutation.props_len != 0)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                BodyRecord::Node* widget_node = nullptr;
                if (target_node != nullptr && target_node->widget == mutation.widget) {
                    widget_node = target_node;
                } else {
                    const auto found =
                        std::ranges::find_if(staged, [&](const BodyRecord::Node& node) {
                            return node.widget == mutation.widget;
                        });
                    if (found != staged.end())
                        widget_node = &*found;
                }
                if (widget_node == nullptr)
                    return SAO_STATUS_ERR_NOT_FOUND;
                const sao_status_t widget_status = validate_body_widget(widget_node->widget);
                if (widget_status != SAO_STATUS_OK)
                    return widget_status;
                const json patch = mutation.props_len == 0
                                       ? json::object()
                                       : json::parse(mutation.props_json_utf8,
                                                     mutation.props_json_utf8 + mutation.props_len);
                if (!patch.is_object())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                for (auto property = patch.begin(); property != patch.end(); ++property)
                    widget_node->props[property.key()] = property.value();
                if (sao::ui::detail::is_typed_widget_handle(widget_node->widget))
                    typed_props.push_back({widget_node->widget, patch});
                break;
            }
            default:
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }

        std::vector<const BodyRecord::Node*> ordered;
        sao_status_t status = order_model(staged, &ordered);
        if (status != SAO_STATUS_OK)
            return status;
        std::vector<std::string> props(ordered.size());
        std::vector<std::string> rollback_props(ordered.size());
        std::vector<SaoUiPanelBodyModelNode> runtime_model(ordered.size());
        std::vector<std::pair<sao_ui_layout_node_handle_t, uint64_t>> next_actual_to_model(
            ordered.size());
        if (mutation_count > UINT64_MAX - current.mutation_count_total)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (mutation_count > current.mutation_log.max_size() - current.mutation_log.size())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        current.mutation_log.reserve(current.mutation_log.size() + mutation_count);
        for (size_t index = 0; index < ordered.size(); ++index) {
            const BodyRecord::Node& node = *ordered[index];
            props[index] = node.props.dump();
            rollback_props[index] = node.committed_props.dump();
            const bool typed_widget =
                node.widget != nullptr && sao::ui::detail::is_typed_widget_handle(node.widget);
            runtime_model[index] = {node.id,
                                    node.parent_id,
                                    node.layout_mode,
                                    node.spec,
                                    node.widget,
                                    typed_widget
                                        ? nullptr
                                        : reinterpret_cast<const uint8_t*>(props[index].data()),
                                    typed_widget ? 0 : props[index].size(),
                                    typed_widget ? nullptr
                                                 : reinterpret_cast<const uint8_t*>(
                                                       rollback_props[index].data()),
                                    typed_widget ? 0 : rollback_props[index].size()};
        }
        for (auto& node : staged)
            node.committed_props = node.props;

        std::vector<sao_ui_layout_node_handle_t> actual(ordered.size());
        sao_ui_layout_tree_handle_t replacement_tree = nullptr;

        struct AppliedTypedProps {
            sao_ui_widget_handle_t widget{};
            sao::ui::detail::WidgetPropsSnapshot snapshot;
        };
        std::vector<AppliedTypedProps> applied_typed_props;
        applied_typed_props.reserve(typed_props.size());
        const auto rollback_typed_props = [&]() {
            sao_status_t first_failure = SAO_STATUS_OK;
            for (auto applied = applied_typed_props.rbegin();
                 applied != applied_typed_props.rend(); ++applied) {
                const sao_status_t rollback_status =
                    sao::ui::detail::restore_typed_widget_props(applied->widget,
                                                                applied->snapshot);
                if (first_failure == SAO_STATUS_OK && rollback_status != SAO_STATUS_OK)
                    first_failure = rollback_status;
            }
            return first_failure;
        };
        for (const auto& patch : typed_props) {
            sao::ui::detail::WidgetPropsSnapshot snapshot;
            status = sao::ui::detail::apply_typed_widget_props(patch.widget, patch.props,
                                                               &snapshot);
            if (status != SAO_STATUS_OK) {
                return rollback_typed_props() == SAO_STATUS_OK
                           ? status
                           : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
            }
            applied_typed_props.push_back({patch.widget, std::move(snapshot)});
        }

        status = sao_ui_panel_replace_body_model(owner->runtime_panel, runtime_model.data(),
                                                 runtime_model.size(), actual.data(), actual.size(),
                                                 &replacement_tree);
        if (status != SAO_STATUS_OK) {
            return rollback_typed_props() == SAO_STATUS_OK
                       ? status
                       : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
        }

        for (size_t index = 0; index < ordered.size(); ++index) {
            next_actual_to_model[index] = {actual[index], ordered[index]->id};
        }
        current.actual_to_model = std::move(next_actual_to_model);
        current.model = std::move(staged);
        current.next_node_id = next_id;
        current.tree = replacement_tree;
        current.root = actual.front();
        for (size_t index = 0; index < mutation_count; ++index) {
            current.mutation_log.push_back(mutations[index].kind);
            if (mutations[index].out_new_node == nullptr || created_by_mutation[index] == 0)
                continue;
            const auto found = std::ranges::find_if(ordered, [&](const BodyRecord::Node* node) {
                return node->id == created_by_mutation[index];
            });
            if (found != ordered.end()) {
                *mutations[index].out_new_node =
                    actual[static_cast<size_t>(found - ordered.begin())];
            }
        }
        current.mutation_count_total += mutation_count;
        return SAO_STATUS_OK;
    } catch (const json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Show / hide / z-order / opacity ─────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_show(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const sao_status_t status = sao_ui_panel_set_visible(rec->runtime_panel, true);
        if (status != SAO_STATUS_OK)
            return status;
        rec->visible = true;
        rec->descriptor_cache.visible = true;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_hide(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const sao_status_t status = sao_ui_panel_set_visible(rec->runtime_panel, false);
        if (status != SAO_STATUS_OK)
            return status;
        rec->visible = false;
        rec->descriptor_cache.visible = false;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_bring_to_front(sao_ui_panel_handle_t panel) {
    try {
        std::lock_guard z_lock(z_order_mutex());
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::vector<std::shared_ptr<PanelRecord>> snapshot;
        {
            auto& reg = registry();
            std::lock_guard registry_lock(reg.mu);
            snapshot.reserve(reg.panels.size());
            for (const auto& [unused, candidate] : reg.panels)
                snapshot.push_back(candidate);
        }
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        int32_t highest = rec->z_within_class;
        for (const auto& candidate : snapshot) {
            if (candidate.get() == rec.get())
                continue;
            std::lock_guard candidate_lock(candidate->mutex);
            if (candidate->active && candidate->z_class == rec->z_class)
                highest = std::max(highest, candidate->z_within_class);
        }
        if (highest >= 250'000'000)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        const int32_t replacement = highest + 1;
        const sao_ui_layer_handle_t layer = sao_ui_panel_layer(rec->runtime_panel);
        if (layer != nullptr) {
            const sao_status_t status =
                sao_ui_layer_set_z_order(layer, global_z_key(rec->z_class, replacement));
            if (status != SAO_STATUS_OK)
                return status;
        }
        rec->z_within_class = replacement;
        rec->descriptor_cache.z_within_class = replacement;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_send_to_back(sao_ui_panel_handle_t panel) {
    try {
        std::lock_guard z_lock(z_order_mutex());
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::vector<std::shared_ptr<PanelRecord>> snapshot;
        {
            auto& reg = registry();
            std::lock_guard registry_lock(reg.mu);
            snapshot.reserve(reg.panels.size());
            for (const auto& [unused, candidate] : reg.panels)
                snapshot.push_back(candidate);
        }
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        int32_t lowest = rec->z_within_class;
        for (const auto& candidate : snapshot) {
            if (candidate.get() == rec.get())
                continue;
            std::lock_guard candidate_lock(candidate->mutex);
            if (candidate->active && candidate->z_class == rec->z_class)
                lowest = std::min(lowest, candidate->z_within_class);
        }
        if (lowest <= -250'000'000)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        const int32_t replacement = lowest - 1;
        const sao_ui_layer_handle_t layer = sao_ui_panel_layer(rec->runtime_panel);
        if (layer != nullptr) {
            const sao_status_t status =
                sao_ui_layer_set_z_order(layer, global_z_key(rec->z_class, replacement));
            if (status != SAO_STATUS_OK)
                return status;
        }
        rec->z_within_class = replacement;
        rec->descriptor_cache.z_within_class = replacement;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_opacity(sao_ui_panel_handle_t panel,
                                                             float opacity_0_to_1) {
    if (!std::isfinite(opacity_0_to_1))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        opacity_0_to_1 = std::clamp(opacity_0_to_1, 0.0F, 1.0F);
        const sao_ui_layer_handle_t layer = sao_ui_panel_layer(rec->runtime_panel);
        if (layer != nullptr) {
            const sao_status_t status = sao_ui_layer_set_alpha(layer, opacity_0_to_1);
            if (status != SAO_STATUS_OK)
                return status;
        }
        rec->opacity_0_to_1 = opacity_0_to_1;
        rec->descriptor_cache.initial_opacity = opacity_0_to_1;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Descriptor readback + registry iteration ────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_get_descriptor(sao_ui_panel_handle_t panel, SaoPanelDescriptor* descriptor_out) {
    if (descriptor_out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        rebuild_descriptor_cache(*rec);
        *descriptor_out = rec->descriptor_cache;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_iterate(sao_ui_panel_registry_iterate_cb_t callback, void* user_data) {
    if (callback == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        struct Item {
            sao_ui_panel_handle_t handle{};
            SaoPanelDescriptor descriptor{};
            std::string panel_id;
            std::string title;
            std::string follow_panel_id;
            std::string theme_override;
            std::vector<uint8_t> icon;
        };
        std::vector<std::pair<sao_ui_panel_handle_t, std::shared_ptr<PanelRecord>>> records;
        {
            auto& reg = registry();
            std::lock_guard guard(reg.mu);
            records.reserve(reg.panels.size());
            for (const auto& [handle, rec] : reg.panels)
                records.emplace_back(handle, rec);
        }
        std::vector<Item> snapshot;
        snapshot.reserve(records.size());
        for (const auto& [handle, rec] : records) {
            std::lock_guard lock(rec->mutex);
            if (!rec->active)
                continue;
            snapshot.emplace_back();
            Item& item = snapshot.back();
            item.handle = handle;
            item.descriptor = rec->descriptor_cache;
            item.panel_id = rec->panel_id;
            item.title = rec->title;
            item.follow_panel_id = rec->follow_panel_id;
            item.theme_override = rec->theme_override_json;
            item.icon = rec->icon_pixels;
            item.descriptor.panel_id_utf8 = item.panel_id.empty() ? nullptr : item.panel_id.c_str();
            item.descriptor.title_utf8 = item.title.empty() ? nullptr : item.title.c_str();
            item.descriptor.follow_panel_id_utf8 =
                item.follow_panel_id.empty() ? nullptr : item.follow_panel_id.c_str();
            item.descriptor.theme_override_json_utf8 =
                item.theme_override.empty() ? nullptr : item.theme_override.c_str();
            item.descriptor.icon_bgra_pixels = item.icon.empty() ? nullptr : item.icon.data();
        }
        for (auto& item : snapshot) {
            try {
                callback(item.handle, &item.descriptor, user_data);
            } catch (...) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_registry_count(size_t* count_out) {
    if (count_out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        reap_retired_panels();
        auto& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mu);
        *count_out = reg.panels.size();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_panel_retired_geometry_count_() {
    try {
        reap_retired_panels();
        auto& reg = registry();
        std::lock_guard lock(reg.mu);
        return reg.retired.size();
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_panel_test_set_publish_failure_point(int32_t point) {
    g_panel_publish_failure_point.store(point, std::memory_order_release);
}

extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_panel_geometry_worker_count_() {
    return g_geometry_worker_count.load(std::memory_order_acquire);
}

// ─── Body inspection (test rig + downstream layout wire-up) ──────────

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_body_get_root(sao_ui_panel_body_handle_t body, sao_ui_layout_node_handle_t* out_root) {
    if (out_root == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_root = nullptr;
    try {
        const auto owner = registered_body_owner(body);
        if (owner == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(owner->mutex);
        if (!owner->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        *out_root = owner->body->root;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_body_get_tree(sao_ui_panel_body_handle_t body, sao_ui_layout_tree_handle_t* out_tree) {

    if (out_tree == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_tree = nullptr;
    try {
        const auto owner = registered_body_owner(body);
        if (owner == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(owner->mutex);
        if (!owner->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        *out_tree = owner->body->tree;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_body_set_spec(sao_ui_panel_body_handle_t body,
                                                               const uint8_t* spec_json_utf8,
                                                               size_t spec_len) {

    if (body == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (spec_json_utf8 == nullptr && spec_len != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto owner = registered_body_owner(body);
        if (owner == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard lock(owner->mutex);
        if (!owner->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_status_t status = sao_ui_panel_set_spec(owner->runtime_panel, spec_json_utf8, spec_len);
        if (status != SAO_STATUS_OK)
            return status;
        status = sao_ui_panel_get_layout_tree(owner->runtime_panel, &owner->body->tree,
                                              &owner->body->root);
        if (status != SAO_STATUS_OK)
            return status;
        owner->body->model.clear();
        owner->body->actual_to_model.clear();
        owner->body->next_node_id = 2;
        initialize_body_model(*owner->body);
        owner->body->mutation_log.push_back(SAO_UI_BODY_UPDATE_SPEC);
        owner->body->mutation_count_total += 1;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_theme_override(
    sao_ui_panel_handle_t panel, const uint8_t* override_json_utf8, size_t override_len) {
    if (override_json_utf8 == nullptr && override_len != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        sao_ui_panel_handle_t runtime_panel = nullptr;
        {
            std::lock_guard lock(rec->mutex);
            if (!rec->active)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            runtime_panel = rec->runtime_panel;
        }
        const sao_status_t apply_status = sao_ui_panel_apply_theme_override_(
            runtime_panel, override_json_utf8, override_len);
        if (apply_status != SAO_STATUS_OK)
            return apply_status;
        std::lock_guard lock(rec->mutex);
        if (!rec->active)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (override_json_utf8 == nullptr || override_len == 0) {
            rec->theme_override_json.clear();
        } else {
            rec->theme_override_json.assign(reinterpret_cast<const char*>(override_json_utf8),
                                            override_len);
        }
        rebuild_descriptor_cache(*rec);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_clear_theme_override(sao_ui_panel_handle_t panel) {
    return sao_ui_panel_set_theme_override(panel, nullptr, 0);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry_persist_handler(
    sao_ui_panel_handle_t panel, sao_ui_panel_geometry_cb_t callback, void* user_data) {

    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        {
            std::lock_guard lock(rec->mutex);
            if (!rec->active)
                return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        rec->geometry_persistence.set_handler(callback, user_data);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ─── Test-only introspection (not exported outside the DLL) ──────────
//
// Exposed via distinct symbols so the Catch2 rig can peek at internal
// state without breaking encapsulation.  Not part of the ABI-stable
// surface; use only from within the same DLL/test executable pair.

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_panel_body_mutation_count(sao_ui_panel_body_handle_t body) {
    try {
        const auto owner = registered_body_owner(body);
        if (owner == nullptr)
            return 0;
        std::lock_guard lock(owner->mutex);
        return owner->active ? owner->body->mutation_count_total : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_body_mutation_at(sao_ui_panel_body_handle_t body, size_t idx) {
    try {
        const auto owner = registered_body_owner(body);
        if (owner == nullptr)
            return -1;
        std::lock_guard lock(owner->mutex);
        if (!owner->active || idx >= owner->body->mutation_log.size())
            return -1;
        return owner->body->mutation_log[idx];
    } catch (...) {
        return -1;
    }
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_panel_z_within_class(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return INT32_MIN;
        std::lock_guard lock(rec->mutex);
        return rec->active ? rec->z_within_class : INT32_MIN;
    } catch (...) {
        return INT32_MIN;
    }
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_panel_is_visible(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return false;
        std::lock_guard lock(rec->mutex);
        return rec->active && rec->visible;
    } catch (...) {
        return false;
    }
}

extern "C" SAO_UI_API float SAO_UI_CALL sao_ui_panel_get_opacity_(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return -1.0F;
        std::lock_guard lock(rec->mutex);
        return rec->active ? rec->opacity_0_to_1 : -1.0F;
    } catch (...) {
        return -1.0F;
    }
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_panel_global_z_key_(sao_ui_panel_handle_t panel) {
    try {
        const auto rec = registered_panel(panel);
        if (rec == nullptr)
            return INT32_MIN;
        std::lock_guard lock(rec->mutex);
        return rec->active ? global_z_key(rec->z_class, rec->z_within_class) : INT32_MIN;
    } catch (...) {
        return INT32_MIN;
    }
}
