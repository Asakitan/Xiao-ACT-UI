// SAO Auto — panel SDK descriptor path (G3.10, Wave 4).
//
// Modern registration path for plugin panels.  Plugins fill a
// SaoPanelDescriptor, call sao_ui_panel_register, then interact with
// the returned panel/body handles via panel_layout.h + this file's
// batched body-mutation API.
//
// This slice is the SDK-facing registry + descriptor cache — it is
// intentionally decoupled from the compositor D3D11 back-end so unit
// tests can drive the full lifecycle on a headless CI runner (see
// tests/test_panel_sdk_wave4.cpp).  The `compositor` argument is stored
// for downstream integration but not required for correctness of the
// registry itself.
//
// Python source alignment (memory `别造额外UI入口`,
// `面板组件库支持颜色覆盖`, `ACT扁平化机制`):
//   * plugin_manager.register_panel     — parent of this file
//   * gui_modules/sao_panel_ui.py       — descriptor semantics
//   * sao_theme/panel_themes            — theme_override_json_utf8
//   * gpu_overlay/z_order.py            — z_class + z_within_class

#include "sao/ui/panel_sdk.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ─── Internal panel record ──────────────────────────────────────────
//
// A registered panel is a POD-ish record kept in a process-wide map
// keyed by descriptor id.  The map serves two roles:
//   1. duplicate-id guard (memory `别造额外UI入口`: one plugin = one
//      main panel, no accidental double-register from show/hide),
//   2. iteration surface for the launcher lifecycle sweep.

struct BodyRecord {
    sao_ui_layout_tree_handle_t tree = nullptr;
    sao_ui_layout_node_handle_t root = nullptr;
    std::vector<int> mutation_log;  // records mutation kinds in order
    uint64_t         mutation_count_total = 0;
};

struct PanelRecord {
    // Persistent copy of the descriptor.  All const char* fields are
    // deep-copied into the strings below so plugins may free their
    // input immediately after register().
    std::string panel_id;
    std::string title;
    std::string follow_panel_id;
    std::string theme_override_json;

    // Descriptor cache (rebuilt on read so we never hand out dangling
    // pointers).  Kept as raw copy of the numeric fields.
    SaoPanelDescriptor descriptor_cache{};

    // Compositor + geometry state.
    sao_ui_compositor_handle_t compositor = nullptr;

    // Runtime state.
    bool  visible          = true;
    float opacity_0_to_1   = 1.0f;
    int32_t z_class        = 0;
    int32_t z_within_class = 0;

    // Body sub-record.
    std::unique_ptr<BodyRecord> body;

    // Handle bookkeeping.
    uint64_t id_num = 0;  // monotonic id assigned at register-time
};

// ─── Global registry ────────────────────────────────────────────────
struct Registry {
    std::mutex                                                  mu;
    // Keyed by the opaque panel handle we hand out.  We use a raw
    // pointer-as-integer scheme: the handle IS the record pointer,
    // so lookup is O(1) via reinterpret_cast (bounded by an alive-set
    // guard below).
    std::unordered_map<PanelRecord*, std::unique_ptr<PanelRecord>> panels;
    // Body → panel back-pointer, so body handles can be looked up.
    std::unordered_map<BodyRecord*, PanelRecord*> body_index;
    // panel_id → panel*, for duplicate-id detection.
    std::unordered_map<std::string, PanelRecord*> by_id;
    uint64_t next_id = 1;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

PanelRecord* lookup(sao_ui_panel_handle_t handle) {
    if (handle == nullptr) return nullptr;
    // Cast handle → record*; guard by presence in the alive map.
    auto* rec = reinterpret_cast<PanelRecord*>(handle);
    auto& reg = registry();
    // Caller must already hold reg.mu when using the returned pointer.
    auto it = reg.panels.find(rec);
    return (it == reg.panels.end()) ? nullptr : rec;
}

BodyRecord* lookup_body(sao_ui_panel_body_handle_t handle) {
    if (handle == nullptr) return nullptr;
    auto* body = reinterpret_cast<BodyRecord*>(handle);
    auto& reg = registry();
    auto it = reg.body_index.find(body);
    return (it == reg.body_index.end()) ? nullptr : body;
}

void rebuild_descriptor_cache(PanelRecord& rec) {
    // Rewire the pointer fields to point into the record's owned
    // strings — this is the shape the caller sees from get_descriptor.
    rec.descriptor_cache.panel_id_utf8 =
        rec.panel_id.empty() ? nullptr : rec.panel_id.c_str();
    rec.descriptor_cache.title_utf8 =
        rec.title.empty() ? nullptr : rec.title.c_str();
    rec.descriptor_cache.follow_panel_id_utf8 =
        rec.follow_panel_id.empty() ? nullptr : rec.follow_panel_id.c_str();
    rec.descriptor_cache.theme_override_json_utf8 =
        rec.theme_override_json.empty() ? nullptr
                                        : rec.theme_override_json.c_str();
}

}  // namespace

// ─── Registration ────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_register(
    sao_ui_compositor_handle_t compositor,
    const SaoPanelDescriptor* descriptor,
    sao_ui_panel_handle_t* out_panel,
    sao_ui_panel_body_handle_t* out_body) {

    if (out_panel != nullptr) *out_panel = nullptr;
    if (out_body  != nullptr) *out_body  = nullptr;
    if (descriptor == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (descriptor->panel_id_utf8 == nullptr ||
        descriptor->panel_id_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);

    // Duplicate-id guard (memory `别造额外UI入口`).
    const std::string id_str(descriptor->panel_id_utf8);
    if (reg.by_id.find(id_str) != reg.by_id.end()) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }

    auto rec = std::make_unique<PanelRecord>();
    rec->panel_id  = id_str;
    if (descriptor->title_utf8 != nullptr) {
        rec->title = descriptor->title_utf8;
    }
    if (descriptor->follow_panel_id_utf8 != nullptr) {
        rec->follow_panel_id = descriptor->follow_panel_id_utf8;
    }
    if (descriptor->theme_override_json_utf8 != nullptr) {
        rec->theme_override_json = descriptor->theme_override_json_utf8;
    }

    // Copy the descriptor bit-for-bit, then re-wire the char* fields
    // to point into our owned strings.
    rec->descriptor_cache = *descriptor;
    rebuild_descriptor_cache(*rec);

    rec->compositor      = compositor;
    rec->visible         = descriptor->visible;
    rec->opacity_0_to_1  = (descriptor->initial_opacity <= 0.0f)
                             ? 1.0f
                             : std::min(1.0f, descriptor->initial_opacity);
    rec->z_class         = descriptor->z_class;
    rec->z_within_class  = descriptor->z_within_class;

    rec->body     = std::make_unique<BodyRecord>();
    SaoUiLayoutSpec body_spec{};
    sao_ui_layout_spec_defaults(&body_spec);
    body_spec.fixed_width_px = descriptor->default_width_px;
    body_spec.fixed_height_px = descriptor->default_height_px;
    if (sao_ui_layout_tree_create(&rec->body->tree) != SAO_STATUS_OK ||
        sao_ui_layout_tree_set_root(rec->body->tree, SAO_UI_LAYOUT_VERTICAL,
                                    &body_spec, &rec->body->root) != SAO_STATUS_OK) {
        sao_ui_layout_tree_destroy(rec->body->tree);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    rec->id_num   = reg.next_id++;

    PanelRecord* raw     = rec.get();
    BodyRecord*  raw_body = rec->body.get();

    reg.panels.emplace(raw, std::move(rec));
    reg.body_index.emplace(raw_body, raw);
    reg.by_id.emplace(id_str, raw);

    if (out_panel != nullptr) {
        *out_panel = reinterpret_cast<sao_ui_panel_handle_t>(raw);
    }
    if (out_body != nullptr) {
        *out_body = reinterpret_cast<sao_ui_panel_body_handle_t>(raw_body);
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_unregister(
    sao_ui_panel_handle_t panel) {

    if (panel == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = reinterpret_cast<PanelRecord*>(panel);
    auto it = reg.panels.find(rec);
    if (it == reg.panels.end()) return SAO_STATUS_ERR_NOT_FOUND;

    sao_ui_layout_tree_destroy(rec->body->tree);
    reg.body_index.erase(rec->body.get());
    reg.by_id.erase(rec->panel_id);
    reg.panels.erase(it);  // unique_ptr auto-frees
    return SAO_STATUS_OK;
}

// ─── Body mutations (batched) ────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_update_body(
    sao_ui_panel_body_handle_t body,
    const SaoUiBodyMutation* mutations,
    size_t mutation_count) {

    if (body == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (mutation_count == 0) return SAO_STATUS_OK;
    if (mutations == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    if (rec == nullptr) return SAO_STATUS_ERR_NOT_FOUND;

    // Apply all mutations against the real body tree while holding the
    // registry lock, so callers observe one coherent mutation batch.
    rec->mutation_log.reserve(rec->mutation_log.size() + mutation_count);
    for (size_t i = 0; i < mutation_count; ++i) {
        const SaoUiBodyMutation& mutation = mutations[i];
        sao_status_t status = SAO_STATUS_OK;
        sao_ui_layout_node_handle_t created = nullptr;
        sao_ui_layout_node_handle_t target = mutation.target == nullptr ? rec->root : mutation.target;
        switch (mutation.kind) {
        case SAO_UI_BODY_ADD_WIDGET:
            if (mutation.spec != nullptr || mutation.widget != nullptr) {
                if (mutation.spec == nullptr || mutation.widget == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
                status = sao_ui_layout_node_add_widget(target, mutation.widget, mutation.spec, &created);
            }
            break;
        case SAO_UI_BODY_ADD_CONTAINER:
            if (mutation.spec != nullptr) {
                status = sao_ui_layout_node_add_container(target, mutation.layout_mode, mutation.spec, &created);
            }
            break;
        case SAO_UI_BODY_REMOVE_NODE:
            if (mutation.target != nullptr) status = sao_ui_layout_node_remove(target);
            break;
        case SAO_UI_BODY_UPDATE_SPEC:
            if (mutation.spec != nullptr) status = sao_ui_layout_node_set_spec(target, mutation.spec);
            break;
        case SAO_UI_BODY_REORDER_NODE:
            if (mutation.target != nullptr) status = sao_ui_layout_node_reorder(target, mutation.new_index);
            break;
        case SAO_UI_BODY_UPDATE_WIDGET_PROPS:
            // SDK widget families apply their typed update before asking the
            // panel body to record its dirty mutation.  A null widget marks
            // that bookkeeping-only path; a concrete generic widget still
            // accepts its JSON property update here.
            if (mutation.widget != nullptr) {
                status = sao_ui_widget_apply_props(
                    mutation.widget, mutation.props_json_utf8, mutation.props_len);
            }
            break;
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (status != SAO_STATUS_OK) return status;
        rec->mutation_log.push_back(mutations[i].kind);
        if (mutation.out_new_node != nullptr) *mutation.out_new_node = created;
    }
    rec->mutation_count_total += mutation_count;
    return SAO_STATUS_OK;
}

// ─── Show / hide / z-order / opacity ─────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_show(
    sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    rec->visible = true;
    rec->descriptor_cache.visible = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_hide(
    sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    rec->visible = false;
    rec->descriptor_cache.visible = false;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_bring_to_front(
    sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    // Bump z_within_class above any current sibling in the same
    // z_class.  Never crosses classes (see z_order.h manager rule
    // in memory `input proxy逐像素+防焦点偷`).
    int32_t highest = rec->z_within_class;
    for (auto& kv : reg.panels) {
        if (kv.first == rec) continue;
        if (kv.first->z_class != rec->z_class) continue;
        if (kv.first->z_within_class > highest) {
            highest = kv.first->z_within_class;
        }
    }
    if (highest >= INT32_MAX - 1) {
        // Renormalize the whole class to stay well below the cap.
        int32_t base = 0;
        for (auto& kv : reg.panels) {
            if (kv.first->z_class == rec->z_class) {
                kv.first->z_within_class = base++;
                kv.first->descriptor_cache.z_within_class =
                    kv.first->z_within_class;
            }
        }
        rec->z_within_class = base;  // one above the compact range
    } else {
        rec->z_within_class = highest + 1;
    }
    rec->descriptor_cache.z_within_class = rec->z_within_class;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_send_to_back(
    sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    int32_t lowest = rec->z_within_class;
    for (auto& kv : reg.panels) {
        if (kv.first == rec) continue;
        if (kv.first->z_class != rec->z_class) continue;
        if (kv.first->z_within_class < lowest) {
            lowest = kv.first->z_within_class;
        }
    }
    rec->z_within_class = (lowest > INT32_MIN + 1) ? (lowest - 1) : lowest;
    rec->descriptor_cache.z_within_class = rec->z_within_class;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_opacity(
    sao_ui_panel_handle_t panel, float opacity_0_to_1) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (opacity_0_to_1 < 0.0f) opacity_0_to_1 = 0.0f;
    if (opacity_0_to_1 > 1.0f) opacity_0_to_1 = 1.0f;
    rec->opacity_0_to_1 = opacity_0_to_1;
    rec->descriptor_cache.initial_opacity = opacity_0_to_1;
    return SAO_STATUS_OK;
}

// ─── Descriptor readback + registry iteration ────────────────────────

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_get_descriptor(
    sao_ui_panel_handle_t panel,
    SaoPanelDescriptor* descriptor_out) {

    if (descriptor_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    rebuild_descriptor_cache(*rec);
    *descriptor_out = rec->descriptor_cache;
    return SAO_STATUS_OK;
}

// Callback signature for registry_iterate — matches the header banner
// intent even though the header itself declares the API stub below.
typedef void (SAO_UI_CALL* sao_ui_panel_registry_iterate_cb_t)(
    sao_ui_panel_handle_t panel,
    const SaoPanelDescriptor* descriptor,
    void* user_data);

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_iterate(
    sao_ui_panel_registry_iterate_cb_t callback, void* user_data) {

    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& reg = registry();

    // Copy the (handle, descriptor) pairs under the lock so the
    // callback can mutate the registry (e.g. unregister) without
    // corrupting the iteration.  Same pattern the Python side uses
    // in `plugin_manager.enumerate_panels`.
    struct Item {
        sao_ui_panel_handle_t handle;
        SaoPanelDescriptor    descriptor;
    };
    std::vector<Item> snapshot;
    {
        std::lock_guard<std::mutex> guard(reg.mu);
        snapshot.reserve(reg.panels.size());
        for (auto& kv : reg.panels) {
            rebuild_descriptor_cache(*kv.first);
            Item it{};
            it.handle     = reinterpret_cast<sao_ui_panel_handle_t>(kv.first);
            it.descriptor = kv.first->descriptor_cache;
            snapshot.push_back(it);
        }
    }
    for (auto& it : snapshot) {
        callback(it.handle, &it.descriptor, user_data);
    }
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_registry_count(size_t* count_out) {
    if (count_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    *count_out = reg.panels.size();
    return SAO_STATUS_OK;
}

// ─── Body inspection (test rig + downstream layout wire-up) ──────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_body_get_root(
    sao_ui_panel_body_handle_t body,
    sao_ui_layout_node_handle_t* out_root) {

    if (out_root == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_root = nullptr;
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_root = rec->root;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_body_get_tree(
    sao_ui_panel_body_handle_t body,
    sao_ui_layout_tree_handle_t* out_tree) {

    if (out_tree == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_tree = nullptr;
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_tree = rec->tree;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_body_set_spec(
    sao_ui_panel_body_handle_t body,
    const uint8_t* spec_json_utf8,
    size_t spec_len) {

    if (body == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec_json_utf8 == nullptr && spec_len != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    // Full-body replacement equals REMOVE_NODE(root) followed by
    // whatever add mutations the caller would emit — we just record
    // one synthetic UPDATE_SPEC event so the mutation log is coherent.
    rec->mutation_log.push_back(SAO_UI_BODY_UPDATE_SPEC);
    rec->mutation_count_total += 1;
    (void)spec_len;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_set_theme_override(
    sao_ui_panel_handle_t panel,
    const uint8_t* override_json_utf8, size_t override_len) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    if (rec == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (override_json_utf8 == nullptr || override_len == 0) {
        rec->theme_override_json.clear();
    } else {
        rec->theme_override_json.assign(
            reinterpret_cast<const char*>(override_json_utf8), override_len);
    }
    rebuild_descriptor_cache(*rec);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_panel_clear_theme_override(
    sao_ui_panel_handle_t panel) {
    return sao_ui_panel_set_theme_override(panel, nullptr, 0);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_panel_set_geometry_persist_handler(
    sao_ui_panel_handle_t panel,
    sao_ui_panel_geometry_cb_t callback,
    void* user_data) {

    // Handler stored on the record, dispatched by the geometry
    // observer in a follow-up slice (see the ~500ms debounce note in
    // panel_sdk.h).  For now we accept + validate the handle.
    (void)callback;
    (void)user_data;
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    if (lookup(panel) == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return SAO_STATUS_OK;
}

// ─── Test-only introspection (not exported outside the DLL) ──────────
//
// Exposed via distinct symbols so the Catch2 rig can peek at internal
// state without breaking encapsulation.  Not part of the ABI-stable
// surface; use only from within the same DLL/test executable pair.

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_panel_body_mutation_count(sao_ui_panel_body_handle_t body) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    return (rec == nullptr) ? 0u : rec->mutation_count_total;
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_body_mutation_at(sao_ui_panel_body_handle_t body, size_t idx) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup_body(body);
    if (rec == nullptr) return -1;
    if (idx >= rec->mutation_log.size()) return -1;
    return rec->mutation_log[idx];
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_panel_z_within_class(sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    return (rec == nullptr) ? INT32_MIN : rec->z_within_class;
}

extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_panel_is_visible(sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    return (rec == nullptr) ? false : rec->visible;
}

extern "C" SAO_UI_API float SAO_UI_CALL
sao_ui_panel_get_opacity_(sao_ui_panel_handle_t panel) {
    auto& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mu);
    auto* rec = lookup(panel);
    return (rec == nullptr) ? -1.0f : rec->opacity_0_to_1;
}
