// pymini_ctx.cpp — binds the canonical ~70-method `ctx` object into pymini
// modules.  Mirrors the legacy `act_platform.plugins.PluginContext` surface
// exactly (same names, defaults, return shapes); each method calls the C ABI
// `sao_plugins_ctx_*` exports (or script_ctx shared helpers for ui.*/load_local).
//
// Callback-carrying registrations wrap PyRef callables in native trampolines;
// the wrapped PyRef is held in the plugin's `cb_keep` list so teardown order
// (loader unregisters → then interpreter dies) is safe.
//
// This file also records every bound name via ctx_surface_note_all so the
// adapter's `runtime_feature:*` advisory check reflects reality.
#include "pymini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"
#include "sao/plugins/script_ctx/menu_navigation.h"

#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_engine.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sao::plugins::pymini {

using sao::plugins::loader::plugin_context_t;
namespace script = sao::plugins::script_ctx;
namespace menu_nav = script::menu_navigation;

namespace {

// ═══ PyRef ↔ nlohmann::json ═══
PyRef py_from_json(interpreter& i, const nlohmann::json& j) {
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return py_none();
    case nlohmann::json::value_t::boolean:
        return py_bool(j.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return py_int(j.get<int64_t>());
    case nlohmann::json::value_t::number_float:
        return py_float(j.get<double>());
    case nlohmann::json::value_t::string:
        return py_str(j.get<std::string>());
    case nlohmann::json::value_t::array: {
        std::vector<PyRef> items;
        items.reserve(j.size());
        for (const auto& x : j)
            items.push_back(py_from_json(i, x));
        return py_list(std::move(items));
    }
    case nlohmann::json::value_t::object: {
        auto d = py_dict();
        for (auto it = j.begin(); it != j.end(); ++it)
            dict_set(as_dict(d), py_str(it.key()), py_from_json(i, it.value()));
        return d;
    }
    default:
        return py_none();
    }
}

nlohmann::json json_from_py(interpreter& i, const PyRef& v,
                            int depth = 0) {
    if (!v || depth > 24)
        return nullptr;
    switch (v->kind) {
    case py_kind::none_:
        return nullptr;
    case py_kind::boolean:
        return as_bool(v)->v;
    case py_kind::integer:
        return as_int(v)->v;
    case py_kind::number:
        return as_float(v)->v;
    case py_kind::string:
        return as_str(v)->v;
    case py_kind::bytes_:
        return as_bytes(v)->v;                    // keeps octets 1:1
    case py_kind::list:
    case py_kind::tuple_: {
        nlohmann::json arr = nlohmann::json::array();
        const auto& items =
            v->kind == py_kind::list ? as_list(v)->v : as_tuple(v)->v;
        for (const auto& x : items)
            arr.push_back(json_from_py(i, x, depth + 1));
        return arr;
    }
    case py_kind::set:
    case py_kind::frozenset: {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& x : as_set(v)->items)
            arr.push_back(json_from_py(i, x, depth + 1));
        return arr;
    }
    case py_kind::dict: {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, x] : as_dict(v)->items)
            obj[py_to_str(i, k)] = json_from_py(i, x, depth + 1);
        return obj;
    }
    case py_kind::instance: {
        // emit __dict__ contents (spec-shaped helper objects)
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, x] : as_dict(as_inst(v)->attrs)->items)
            if (auto* ks = as_str(k))
                obj[ks->v] = json_from_py(i, x, depth + 1);
        return obj;
    }
    default:
        return py_to_str(i, v);
    }
}

std::string jstr(interpreter& i, const PyRef& v, int depth = 0) {
    return json_from_py(i, v, depth).dump();
}

PyRef parse_json(interpreter& i, const char* s) {
    if (!s || !*s)
        return py_none();
    try {
        return py_from_json(i, nlohmann::json::parse(s));
    } catch (...) {
        return py_none();
    }
}

char* take_str(char* raw) { return raw; }         // freed via ctx_free_string

void free_ctx_str(char* s) {
    if (s)
        loader::sao_plugins_ctx_free_string(s);
}

plugin_context_t* need_ctx(interpreter& i) {
    if (!i.cfg.ctx)
        i.raise_exc("RuntimeError", "plugin context is not bound", {});
    return i.cfg.ctx;
}

const char* cstr_arg(interpreter& i, const py_args& a, std::size_t n,
                     const char* fn, const char* dflt = "") {
    if (n >= a.pos.size())
        return dflt;
    if (auto* s = as_str(a.pos[n]))
        return s->v.c_str();
    // non-str positional: marshal via py_to_str into a static? we need a
    // stable pointer — callers that need this keep a std::string alive in a
    // local before calling cstr_arg on it; instead require str.
    i.raise_exc("TypeError",
                std::string(fn) + " arg " + std::to_string(n) + " must be str",
                {});
}

// kw helpers
PyRef kw_get(const py_args& a, const char* name) {
    for (const auto& [k, v] : a.kw)
        if (k == name)
            return v;
    return nullptr;
}
std::string kw_str(const py_args& a, const char* name,
                   const std::string& dflt = "") {
    if (PyRef v = kw_get(a, name))
        if (auto* s = as_str(v))
            return s->v;
    return dflt;
}
double kw_num(const py_args& a, const char* name, double dflt) {
    if (PyRef v = kw_get(a, name)) {
        bool ok = false;
        const double d = py_to_float(v, &ok);
        if (ok)
            return d;
    }
    return dflt;
}
bool kw_bool(interpreter& i, const py_args& a, const char* name, bool dflt) {
    if (PyRef v = kw_get(a, name))
        return i.truthy(v);
    return dflt;
}
PyRef pos_or(const py_args& a, std::size_t n, PyRef dflt = nullptr) {
    return n < a.pos.size() ? a.pos[n] : dflt;
}
std::string pos_str(interpreter& i, const py_args& a, std::size_t n,
                    const std::string& dflt = "") {
    if (n < a.pos.size())
        return py_to_str(i, a.pos[n]);
    return dflt;
}

// ═══ callback keep-list + trampolines ═══
// Each wrapped callback stores a strong PyRef + the interpreter pointer.
// `cb_keep` is cleared when the interpreter dies; loader teardown removes
// registrations first, so no callback can fire after the list is gone.
struct mini_menu {
    menu_nav::State navigation;
    std::unordered_map<std::string, std::string> navigation_keys;
    std::unordered_map<std::string, PyRef> actions;
    std::string provider_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0;
    uint64_t revision = 0;
    bool building = false;
};

struct cb_box {
    interpreter* i = nullptr;
    PyRef fn;
    std::string aux;                            // e.g. panel id
    std::shared_ptr<mini_menu> menu;
};
using cb_ptr = std::unique_ptr<cb_box>;
// registry + teardown: file-scope statics so the host can drop all
// callbacks exactly once when its interpreter dies.
std::mutex g_cb_mu;
std::unordered_map<interpreter*, std::vector<cb_ptr>> g_cb_keep;

// ctx_builder* are declared later in this file; stored as shared_ptr so the
// map can live before the full type. The ctx proxy dict's ~60 bound methods
// capture the raw builder `this`, so the builder must outlive the dict —
// anchor it to the interpreter the same way as keep_cb, dropped together
// with callbacks at pymini_drop_callbacks (helpers keep theirs for the
// interpreter's lifetime).
struct ctx_builder;
std::unordered_map<interpreter*,
                   std::vector<std::shared_ptr<ctx_builder>>>
    g_ctx_builders;

cb_box* keep_cb(interpreter& i, PyRef fn, std::string aux = {}) {
    cb_ptr b = std::make_unique<cb_box>();
    b->i = &i;
    b->fn = std::move(fn);
    b->aux = std::move(aux);
    cb_box* raw = b.get();
    std::lock_guard<std::mutex> g(g_cb_mu);
    g_cb_keep[&i].push_back(std::move(b));
    return raw;
}

} // namespace

// pymini_host calls this at interpreter teardown (after the loader removed
// every registration that could still deliver callbacks).
void pymini_drop_callbacks(interpreter& i) {
    // Deferred destruction outside g_cb_mu: ctx_builder teardown releases
    // its lazily-bound engine SaoSdkContext, whose provider cleanup waits
    // for in-flight engine callbacks to drain; those trampolines may call
    // back into keep_cb → g_cb_mu, and the cb_boxes they reference must
    // stay alive through the drain, so boxes are released after builders.
    std::vector<cb_ptr> boxes;
    std::vector<std::shared_ptr<ctx_builder>> builders;
    {
        std::lock_guard<std::mutex> g(g_cb_mu);
        if (auto it = g_ctx_builders.find(&i); it != g_ctx_builders.end()) {
            builders = std::move(it->second);
            g_ctx_builders.erase(it);
        }
        if (auto it = g_cb_keep.find(&i); it != g_cb_keep.end()) {
            boxes = std::move(it->second);
            g_cb_keep.erase(it);
        }
    }
}

namespace {

std::string mini_menu_hash(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) { hash ^= byte; hash *= 1099511628211ULL; }
    char buffer[17]{};
    const auto result = std::to_chars(buffer, buffer + 16, hash, 16);
    return std::string(buffer, result.ptr);
}

bool mini_menu_callable(interpreter& i, const PyRef& value) {
    if (!value || py_is_none(value)) return false;
    switch (value->kind) {
    case py_kind::func:
    case py_kind::builtin:
    case py_kind::bound_method:
    case py_kind::class_: return true;
    case py_kind::instance: return i.hasattr(value, "__call__");
    default: return false;
    }
}

std::string mini_menu_text(interpreter& i, const PyRef& value, bool required = false) {
    if ((!value || py_is_none(value)) && !required) return {};
    const auto* text = as_str(value);
    if (!text || (required && text->v.empty()) || text->v.size() > 16384 ||
        text->v.find('\0') != std::string::npos)
        i.raise_exc("ValueError", "menu text must be a bounded string without NUL", {});
    return text->v;
}

std::string mini_menu_json(interpreter& i, const PyRef& value) {
    nlohmann::json result;
    struct Frame {
        PyRef value;
        nlohmann::json* output;
        bool exit = false;
        size_t depth = 0;
    };
    std::vector<Frame> stack{{value, &result}};
    std::unordered_set<PyObj*> ancestors;
    size_t nodes = 0, bytes = 0;
    while (!stack.empty()) {
        auto frame = std::move(stack.back());
        stack.pop_back();
        if (frame.exit) { ancestors.erase(frame.value.get()); continue; }
        if (++nodes > 16384 || frame.depth > 64) i.raise_exc("ValueError", "menu payload JSON complexity budget exceeded", {});
        const auto& v = frame.value;
        auto& out = *frame.output;
        if (!v || py_is_none(v)) out = nullptr;
        else if (auto* b = as_bool(v)) out = b->v;
        else if (auto* n = as_int(v)) out = n->v;
        else if (auto* n = as_float(v)) {
            if (!std::isfinite(n->v)) i.raise_exc("ValueError", "non-finite menu JSON", {});
            out = n->v;
        } else if (auto* s = as_str(v)) {
            if (s->v.size() > menu_nav::max_text_bytes - bytes) i.raise_exc("ValueError", "menu JSON byte budget exceeded", {});
            bytes += s->v.size();
            out = s->v;
        }
        else if (as_list(v) || as_tuple(v) || as_dict(v)) {
            if (!ancestors.insert(v.get()).second) i.raise_exc("ValueError", "cyclic menu JSON", {});
            stack.push_back({v, frame.output, true});
            if (auto* dict = as_dict(v)) {
                if (dict->items.size() > 16384 - nodes) i.raise_exc("ValueError", "menu JSON node budget exceeded", {});
                out = nlohmann::json::object();
                for (const auto& [key, child] : dict->items) {
                    const auto text = mini_menu_text(i, key);
                    if (text.size() > menu_nav::max_text_bytes - bytes) i.raise_exc("ValueError", "menu JSON byte budget exceeded", {});
                    bytes += text.size();
                    stack.push_back({child, &out[text], false, frame.depth + 1});
                }
            } else {
                const auto& items = as_list(v) ? as_list(v)->v : as_tuple(v)->v;
                if (items.size() > 16384 - nodes) i.raise_exc("ValueError", "menu JSON node budget exceeded", {});
                out = nlohmann::json::array();
                auto& array = out.get_ref<nlohmann::json::array_t&>();
                array.resize(items.size());
                for (size_t index = 0; index < items.size(); ++index)
                    stack.push_back({items[index], &array[index], false, frame.depth + 1});
            }
        } else i.raise_exc("TypeError", "menu JSON value is not serializable", {});
        if (bytes > menu_nav::max_text_bytes) i.raise_exc("ValueError", "menu JSON byte budget exceeded", {});
    }
    const auto encoded = result.dump();
    if (encoded.size() > menu_nav::max_text_bytes ||
        !sdk_binding::sao_plugins_binding_validate_json_text(
            reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()))
        i.raise_exc("ValueError", "menu JSON exceeds its complexity budget", {});
    return encoded;
}

void build_mini_menu(cb_box& box, const menu_nav::State* selected = nullptr,
                     const std::vector<std::string>* selection = nullptr) {
    auto& i = *box.i;
    auto& menu = *box.menu;
    if (menu.building) i.raise_exc("RuntimeError", "menu builder is already active", {});
    menu.building = true;
    struct Guard { bool& active; ~Guard() { active = false; } } guard{menu.building};
    auto navigation = selected ? *selected : menu.navigation;
    const auto desired_path = selection ? *selection : navigation.path();
    auto actions = menu.actions;
    struct InputGuard {
        std::vector<PyRef> values;
        ~InputGuard() { for (auto& value : values) value.reset(); }
    } inputs;
    inputs.values.reserve(3 * (menu_nav::max_nodes + 1));
    std::vector<menu_nav::Node> tree;
    std::vector<menu_nav::Node*> tree_nodes;
    tree_nodes.reserve(menu_nav::max_nodes);
    struct TreeGuard {
        std::vector<menu_nav::Node*>& nodes;
        ~TreeGuard() {
            for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) (*node)->children.clear();
        }
    } tree_guard{tree_nodes};
    struct Frame {
        PyRef source, owner, builder;
        std::vector<PyRef> items;
        std::vector<menu_nav::Node>* nodes;
        std::string scope;
        bool selected;
        size_t index = 0;
        std::unordered_map<std::string, size_t> occurrences;
        std::unordered_set<std::string> keys;
    };
    std::vector<Frame> stack;
    std::unordered_set<PyObj*> ancestors;
    size_t count = 0, bytes = 0;
    const auto push = [&](PyRef source, PyRef owner, PyRef builder,
                          std::vector<menu_nav::Node>* nodes, std::string scope, bool on_path) {
        if (!as_list(source) && !as_tuple(source))
            i.raise_exc("TypeError", "menu children must be a list or tuple", {});
        if (ancestors.contains(source.get()) || (owner && ancestors.contains(owner.get())) ||
            (builder && ancestors.contains(builder.get())))
            i.raise_exc("ValueError", "cyclic menu sequence, mapping or builder", {});
        const auto& items = as_list(source) ? as_list(source)->v : as_tuple(source)->v;
        if (items.size() > menu_nav::max_nodes - count) i.raise_exc("ValueError", "menu node budget exceeded", {});
        count += items.size();
        inputs.values.push_back(owner);
        inputs.values.push_back(source);
        inputs.values.push_back(builder);
        nodes->reserve(items.size());
        ancestors.insert(source.get());
        if (owner) ancestors.insert(owner.get());
        if (builder) ancestors.insert(builder.get());
        auto snapshot = items;
        stack.push_back({std::move(source), std::move(owner), std::move(builder), std::move(snapshot),
                         nodes, std::move(scope), on_path});
    };
    push(i.call0(box.fn, {}), {}, box.fn, &tree, menu.provider_id, true);
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.index == frame.items.size()) {
            ancestors.erase(frame.source.get());
            if (frame.owner) ancestors.erase(frame.owner.get());
            if (frame.builder) ancestors.erase(frame.builder.get());
            stack.pop_back();
            continue;
        }
        const auto item = frame.items[frame.index++];
        if (!as_dict(item)) i.raise_exc("TypeError", "menu rows must be dictionaries", {});
        if (ancestors.contains(item.get())) i.raise_exc("ValueError", "cyclic menu dictionary", {});
        const auto get = [&](const char* key) { return dict_get(as_dict(item), py_str(key)); };
        const auto flag = [&](const char* key, bool fallback) {
            const auto value = get(key);
            return value ? i.truthy(value) : fallback;
        };
        menu_nav::Node node;
        auto& row = node.row;
        row.label = mini_menu_text(i, get("label"), true);
        row.icon = mini_menu_text(i, get("icon"));
        if (auto encoded = get("payload_json")) {
            row.payload_json = mini_menu_text(i, encoded, true);
            if (!sdk_binding::sao_plugins_binding_validate_json_text(
                    reinterpret_cast<const uint8_t*>(row.payload_json.data()), row.payload_json.size()))
                i.raise_exc("ValueError", "invalid menu payload_json", {});
        } else if (auto payload = get("payload")) row.payload_json = mini_menu_json(i, payload);
        row.keep_open = flag("keep_menu_open", false);
        row.close_before = flag("close_menu_before", false);
        PyRef children;
        for (const char* alias : {"children", "items", "submenu"}) {
            const auto value = get(alias);
            if (!value || py_is_none(value)) continue;
            if (children && children != value) i.raise_exc("ValueError", "ambiguous submenu aliases", {});
            children = value;
        }
        node.submenu = bool(children);
        auto command = get("command");
        const bool callable = command && !py_is_none(command);
        if (callable && !mini_menu_callable(i, command)) i.raise_exc("TypeError", "menu command must be callable or None", {});
        if (callable && node.submenu) i.raise_exc("ValueError", "submenu also has a leaf command", {});
        row.can_activate = flag("can_activate", true) && (callable || node.submenu);
        auto explicit_id = get("action_id");
        if (!explicit_id) explicit_id = get("id");
        std::string identity = explicit_id ? mini_menu_text(i, explicit_id, true) : std::string{};
        if (identity.starts_with(menu_nav::navigation_prefix)) i.raise_exc("ValueError", "reserved menu action identity", {});
        if (identity.empty()) {
            const auto append = [&](const std::string& text) { identity += std::to_string(text.size()) + ":" + text; };
            PyRef target = command;
            if (target && target->kind == py_kind::bound_method)
                target = static_cast<PyBoundMethodObj*>(target.get())->fn;
            append(as_func(target) ? as_func(target)->name : as_builtin(target) ? as_builtin(target)->name : "");
            append(row.label);
            append(row.icon);
            append(row.payload_json);
            identity += row.can_activate ? '1' : '0';
            identity += row.keep_open ? '1' : '0';
            identity += row.close_before ? '1' : '0';
            const auto occurrence = frame.occurrences[identity]++;
            identity += "#" + std::to_string(occurrence);
        }
        node.key = mini_menu_hash(identity);
        if (!frame.keys.insert(node.key).second) i.raise_exc("ValueError", "duplicate sibling menu identity", {});
        const auto scope = mini_menu_hash(frame.scope + "\n" + identity);
        row.action_id = "menu-action-" + scope;
        if (callable) {
            actions[row.action_id] = std::move(command);
            if (actions.size() > menu_nav::max_nodes) i.raise_exc("ValueError", "menu action history budget exceeded", {});
        }
        bytes += node.key.size() + row.label.size() + row.icon.size() + row.action_id.size() +
                 row.payload_json.size() + menu.provider_id.size() + menu.name.size() + menu.icon.size();
        if (bytes > menu_nav::max_text_bytes) i.raise_exc("ValueError", "menu byte budget exceeded", {});
        const auto depth = stack.size() - 1;
        const bool on_path = frame.selected && row.can_activate && depth < desired_path.size() && desired_path[depth] == node.key;
        frame.nodes->push_back(std::move(node));
        tree_nodes.push_back(&frame.nodes->back());
        auto* nested = &frame.nodes->back().children;
        if (children) {
            PyRef builder;
            if (mini_menu_callable(i, children)) {
                if (!on_path) continue;
                if (ancestors.contains(children.get())) i.raise_exc("ValueError", "cyclic submenu builder", {});
                builder = std::move(children);
                children = i.call0(builder, {});
            }
            push(std::move(children), item, std::move(builder), nested, scope, on_path);
        }
    }
    std::string error;
    if (!navigation.replace(tree, error)) i.raise_exc("ValueError", error, {});
    const auto* page = &tree;
    for (const auto& key : navigation.path()) {
        const auto found = std::find_if(page->begin(), page->end(), [&](const auto& node) { return node.key == key; });
        if (found == page->end()) i.raise_exc("RuntimeError", "menu navigation path is inconsistent", {});
        page = &found->children;
    }
    std::unordered_map<std::string, std::string> navigation_keys;
    const size_t offset = navigation.path().empty() ? 0 : 1;
    if (offset) navigation_keys.emplace(navigation.rows().front().action_id, std::string{});
    for (size_t index = 0; index < page->size(); ++index)
        if ((*page)[index].submenu)
            navigation_keys.emplace(navigation.rows()[index + offset].action_id, (*page)[index].key);
    auto revision = menu.revision;
    if (revision == 0 || navigation.rows() != menu.navigation.rows()) {
        if (revision == (std::numeric_limits<uint64_t>::max)()) i.raise_exc("OverflowError", "menu revision exhausted", {});
        ++revision;
    }
    menu.actions.swap(actions);
    menu.navigation = std::move(navigation);
    menu.navigation_keys.swap(navigation_keys);
    menu.revision = revision;
}

int32_t SAO_PLUGINS_CALL tr_menu_snapshot(void* rows, uint32_t capacity, uint32_t stride,
    uint32_t* count, uint64_t* revision, loader::entity_snapshot_content_token_t* token,
    uint32_t* output_stride, void* ud) {
    auto* box = static_cast<cb_box*>(ud);
    if (!box || !box->i || !box->menu || !count || !revision || !token || !output_stride ||
        (!rows && (capacity || stride))) return SAO_ERR_INVALID_ARGUMENT;
    gil_guard guard(*box->i);
    try {
        if (!rows) build_mini_menu(*box);
        const auto& menu = *box->menu;
        const auto& page = menu.navigation.rows();
        *count = static_cast<uint32_t>(page.size());
        *revision = menu.revision;
        *token = menu.revision ? menu.revision : 1;
        *output_stride = page.empty() ? 0 : sizeof(loader::entity_menu_row_v2);
        if (capacity < page.size()) return SAO_ERR_BUFFER_TOO_SMALL;
        if (!page.empty() && (stride < sizeof(loader::entity_menu_row_v2) ||
            stride % alignof(loader::entity_menu_row_v2))) return SAO_ERR_INVALID_ARGUMENT;
        for (size_t index = 0; index < page.size(); ++index) {
            const auto& row = page[index];
            const loader::entity_menu_row_v2 native{sizeof(loader::entity_menu_row_v2),
                menu.provider_id.c_str(), menu.name.c_str(), menu.icon.c_str(), menu.priority,
                row.label.c_str(), row.icon.c_str(), row.action_id.c_str(), row.payload_json.c_str(),
                static_cast<uint8_t>(row.can_activate), static_cast<uint8_t>(row.keep_open),
                static_cast<uint8_t>(row.close_before), {}};
            std::memcpy(static_cast<std::byte*>(rows) + index * stride, &native, sizeof(native));
        }
        return SAO_OK;
    } catch (...) { return SAO_ERR_OS_CALL_FAILED; }
}

int32_t SAO_PLUGINS_CALL tr_menu_action(const char* action, const char* payload,
    loader::entity_action_result_sink_v2_fn sink, void* sink_data, void* ud) {
    auto* box = static_cast<cb_box*>(ud);
    if (!box || !box->i || !box->menu || !action || !payload || !sink) return SAO_ERR_INVALID_ARGUMENT;
    gil_guard guard(*box->i);
    try {
        auto& menu = *box->menu;
        if (menu.building) return loader::SAO_PLUGINS_ERR_BUSY;
        auto navigation = menu.navigation;
        const auto result = navigation.activate(action);
        bool handled = false;
        std::string encoded;
        if (result == menu_nav::NavigationResult::handled) {
            const auto route = menu.navigation_keys.find(action);
            if (route == menu.navigation_keys.end()) return SAO_ERR_OS_CALL_FAILED;
            auto selection = menu.navigation.path();
            if (route->second.empty()) {
                if (selection.empty()) return SAO_ERR_OS_CALL_FAILED;
                selection.pop_back();
            } else selection.push_back(route->second);
            build_mini_menu(*box, &navigation, &selection);
            handled = true;
        } else if (result == menu_nav::NavigationResult::not_navigation &&
                   std::any_of(menu.navigation.rows().begin(), menu.navigation.rows().end(),
                       [action](const menu_nav::Row& row) {
                           return row.can_activate && row.action_id == action;
                       })) {
            if (const auto found = menu.actions.find(action); found != menu.actions.end()) {
                const auto callback = found->second;
                const auto value = box->i->call0(callback, {});
                handled = true;
                if (value && !py_is_none(value)) encoded = mini_menu_json(*box->i, value);
            }
        }
        const loader::entity_action_result_v2 native{sizeof(loader::entity_action_result_v2),
            loader::kEntityActionAbiVersion2, static_cast<uint8_t>(handled), {},
            encoded.empty() ? nullptr : encoded.c_str()};
        return sink(&native, sink_data);
    } catch (...) { return SAO_ERR_OS_CALL_FAILED; }
}

// Trampoline entry points — each grabs gil, marshals, calls, swallows.
void tr_event(const char* topic, const char* event_json, void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return;
    gil_guard g(*b->i);
    try {
        PyRef payload = parse_json(*b->i, event_json);
        b->i->call(b->fn, py_args{{py_str(topic ? topic : ""), payload}}, {});
    } catch (...) {
    }
}
void tr_timer(void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return;
    gil_guard g(*b->i);
    try {
        b->i->call0(b->fn, {});
    } catch (...) {
    }
}
void tr_hotkey(void* ud) {
    tr_timer(ud);
}
int32_t tr_render_hook(const char* surface, const char* payload_json,
                       char** out_spec_json, void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn || !out_spec_json)
        return -1;
    *out_spec_json = nullptr;
    gil_guard g(*b->i);
    try {
        PyRef payload = parse_json(*b->i, payload_json);
        PyRef r = b->i->call(
            b->fn, py_args{{py_str(surface ? surface : ""), payload}}, {});
        const std::string s = r ? jstr(*b->i, r) : std::string("{}");
        *out_spec_json = static_cast<char*>(std::malloc(s.size() + 1));
        if (!*out_spec_json)
            return -2;
        std::memcpy(*out_spec_json, s.c_str(), s.size() + 1);
        return 0;
    } catch (...) {
        return -1;
    }
}
int32_t tr_action(const char* action_id, const char* payload_json,
                  char** out_result_json, void* ud) {
    if (action_id && std::string_view(action_id).starts_with(menu_nav::navigation_prefix)) {
        if (!out_result_json) return SAO_ERR_INVALID_ARGUMENT;
        *out_result_json = nullptr;
        return SAO_OK;
    }
    return tr_render_hook(action_id, payload_json, out_result_json, ud);
}
int32_t tr_data_source(void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return 0;
    gil_guard g(*b->i);
    try {
        b->i->call0(b->fn, {});
    } catch (...) {
        return -1;
    }
    return 0;
}

// engine_callback generic channel (binding_engine.h): the cb_box's fn is a
// dict channel→callable filled by ctx.engine.on(); script delivery is
// (channel_str, decoded_payload).  Payload is (ptr,size), not
// NUL-terminated, so it is copied before parse_json.
void SAO_PLUGINS_CALL tr_engine_channel(const char* channel,
                                        const uint8_t* payload_json,
                                        std::size_t payload_size, void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn || !as_dict(b->fn))
        return;
    gil_guard g(*b->i);
    try {
        PyRef cb = dict_get(as_dict(b->fn), py_str(channel ? channel : ""));
        if (!cb)
            return;
        PyRef payload = py_none();
        if (payload_json != nullptr && payload_size > 0) {
            const std::string text(
                reinterpret_cast<const char*>(payload_json), payload_size);
            payload = parse_json(*b->i, text.c_str());
        }
        b->i->call(cb, py_args{{py_str(channel ? channel : ""), payload}},
                   {});
    } catch (...) {
    }
}

// token "topic:hex" parse (v1 shape)
std::string fmt_token(const std::string& tag, uint32_t t) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08x", t);
    return tag + ":" + buf;
}
bool parse_token(const std::string& tok, uint32_t* out) {
    const auto pos = tok.rfind(':');
    if (pos == std::string::npos)
        return false;
    try {
        *out = static_cast<uint32_t>(std::stoul(tok.substr(pos + 1), nullptr, 16));
    } catch (...) {
        return false;
    }
    return true;
}

std::vector<PyRef> mini_named_args(interpreter& i, const py_args& args,
                                 std::initializer_list<const char*> names, size_t required) {
    if (args.pos.size() > names.size()) i.raise_exc("TypeError", "too many arguments", {});
    std::vector<PyRef> values(names.size());
    std::copy(args.pos.begin(), args.pos.end(), values.begin());
    for (const auto& [name, value] : args.kw) {
        auto found = std::find_if(names.begin(), names.end(), [&](const char* key) { return name == key; });
        if (found == names.end()) i.raise_exc("TypeError", "unexpected argument: " + name, {});
        auto& slot = values[static_cast<size_t>(found - names.begin())];
        if (slot) i.raise_exc("TypeError", "duplicate argument: " + name, {});
        slot = value;
    }
    for (size_t index = 0; index < required; ++index)
        if (!values[index]) i.raise_exc("TypeError", "missing required argument", {});
    return values;
}

uint64_t mini_texture_uint(interpreter& i, const PyRef& value, uint64_t maximum, bool handle = false) {
    if (!value) return 0;
    uint64_t number = 0;
    if (const auto* integer = as_int(value)) {
        if (integer->v < 0) i.raise_exc("OverflowError", "texture integer must be unsigned", {});
        number = static_cast<uint64_t>(integer->v);
    } else if (handle && as_str(value)) {
        const auto& text = as_str(value)->v;
        const auto offset = text.starts_with("0x") || text.starts_with("0X") ? 2U : 0U;
        const auto parsed = std::from_chars(text.data() + offset, text.data() + text.size(), number, offset ? 16 : 10);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            i.raise_exc("ValueError", "texture handle must be an exact uint64 decimal or hex string", {});
    } else i.raise_exc("TypeError", "texture handle and dimensions must be integers", {});
    if (number > maximum) i.raise_exc("OverflowError", "texture integer is out of range", {});
    return number;
}

void mini_compositor_status(interpreter& i, int32_t status) {
    if (status != SAO_OK)
        i.raise_exc("RuntimeError", "compositor operation failed: " + std::to_string(status), {});
}

// ═══ ctx builder ═══
struct ctx_builder {
    interpreter& i;
    PyRef ui_obj;
    PyRef engines;          // dict name→py engine object (get_engine lookup)
    PyRef ledgers;          // {"panels": {}, "hotkeys": {}, "timers": {}, ...}
    std::string root_utf8;

    // Reflective engine surface (binding_engine.h).  pymini's canonical ctx
    // is the loader plugin_context_t; sao_plugins_sdk_context_dispatch
    // targets a SaoSdkContext, so the builder lazily binds a per-plugin one
    // via sao_sdk_context_create + sao_sdk_context_bind_platform_services —
    // the same sequence the csharp/csmini/pyhost adapters run.  Binding is
    // deferred to first use so plugins that never touch ctx.engine pay no
    // SharedRuntime slot; engine_sdk_status keeps the creation error stable.
    cb_box* engine_cb_hub = nullptr;  // keep_cb'd dict channel→callable
    SaoSdkContext* engine_sdk = nullptr;
    bool engine_sdk_tried = false;
    int32_t engine_sdk_status = 0;

    ~ctx_builder() {
        if (engine_sdk != nullptr)
            sao_sdk_context_destroy(engine_sdk);  // best-effort + quarantine
    }

    plugin_context_t* c() { return need_ctx(i); }

    PyRef method(const char* n, py_native_fn f) {
        return py_builtin(n, std::move(f));
    }

    // ── simple JSON pass-throughs ────────────────────────────────────
    PyRef m_log(interpreter&, const py_args& a) {
        const std::string msg = a.pos.empty() ? "" : py_to_str(i, a.pos[0]);
        sao_plugins_ctx_log(c(), msg.c_str());
        return py_none();
    }
    PyRef m_emit(interpreter&, const py_args& a) {
        const std::string topic = pos_str(i, a, 0);
        const std::string payload =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("{}");
        sao_plugins_ctx_emit(c(), topic.c_str(), payload.c_str());
        return py_none();
    }
    PyRef m_get_snapshot(interpreter&, const py_args&) {
        char* raw = nullptr;
        if (sao_plugins_ctx_get_snapshot(c(), &raw) != 0 || !raw)
            return py_dict();
        PyRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    PyRef m_snapshot_value(interpreter&, const py_args& a) {
        const std::string path = pos_str(i, a, 0);
        char* raw = nullptr;
        if (sao_plugins_ctx_snapshot_value(c(), path.c_str(), &raw) != 0 ||
            !raw) {
            return pos_or(a, 1, py_none());
        }
        PyRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    PyRef m_recent_events(interpreter&, const py_args& a) {
        bool ok = false;
        const int64_t limit =
            a.pos.empty() ? 20 : py_to_int(a.pos[0], &ok);
        const std::string topic = pos_str(i, a, 1);
        char* raw = nullptr;
        if (sao_plugins_ctx_recent_events(c(), static_cast<uint32_t>(limit),
                                          topic.c_str(), &raw) != 0 ||
            !raw)
            return py_list();
        PyRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    PyRef m_get_setting(interpreter&, const py_args& a) {
        const std::string key = pos_str(i, a, 0);
        char* raw = nullptr;
        if (sao_plugins_ctx_get_setting(c(), key.c_str(), &raw) != 0 || !raw)
            return pos_or(a, 1, py_none());
        PyRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    PyRef m_set_setting(interpreter&, const py_args& a) {
        const std::string key = pos_str(i, a, 0);
        const std::string val =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("null");
        sao_plugins_ctx_set_setting(c(), key.c_str(), val.c_str());
        return py_none();
    }
    PyRef m_set_defaults(interpreter&, const py_args& a) {
        if (!a.pos.empty())
            sao_plugins_ctx_set_defaults(c(), jstr(i, a.pos[0]).c_str());
        return py_none();
    }

    // ── events with callbacks ────────────────────────────────────────
    PyRef m_subscribe(interpreter&, const py_args& a) {
        const std::string topic = pos_str(i, a, 0);
        PyRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("TypeError", "subscribe() missing callback", {});
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_subscribe(c(), topic.c_str(),
                                                    tr_event, b, &token);
        if (r != 0)
            return py_none();
        return py_str(fmt_token(topic, token));
    }
    PyRef m_subscribe_once(interpreter&, const py_args& a) {
        const std::string topic = pos_str(i, a, 0);
        PyRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("TypeError", "subscribe_once() missing callback", {});
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_subscribe_once(c(), topic.c_str(),
                                                         tr_event, b, &token);
        if (r != 0)
            return py_none();
        return py_str(fmt_token(topic, token));
    }
    PyRef m_unsubscribe(interpreter&, const py_args& a) {
        const std::string tok = pos_str(i, a, 0);
        uint32_t t = 0;
        if (parse_token(tok, &t))
            sao_plugins_ctx_unsubscribe(c(), t);
        return py_none();
    }
    // ctx.on(topic) decorator factory
    PyRef m_on(interpreter&, const py_args& a) {
        const std::string topic = pos_str(i, a, 0);
        PyRef maybe_cb = pos_or(a, 1);
        auto do_sub = [this, topic](interpreter&, PyRef cb) -> PyRef {
            cb_box* b = keep_cb(i, cb);
            uint32_t token = 0;
            if (sao_plugins_ctx_subscribe(c(), topic.c_str(), tr_event, b,
                                          &token) != 0)
                return cb;
            return cb;
        };
        if (maybe_cb) {                          // direct form on(topic, cb)
            (void)do_sub(i, maybe_cb);
            return maybe_cb;
        }
        // decorator form: returns fn(cb)→cb
        return py_builtin("on_decorator",
                          [this, topic](interpreter&, const py_args& a2) {
                              PyRef cb = a2.pos.empty() ? nullptr : a2.pos[0];
                              if (cb) {
                                  cb_box* b = keep_cb(i, cb);
                                  uint32_t token = 0;
                                  sao_plugins_ctx_subscribe(
                                      c(), topic.c_str(), tr_event, b,
                                      &token);
                              }
                              return cb ? cb : py_none();
                          });
    }
    PyRef on_topic(interpreter&, const py_args& a, const char* topic) {
        // on_damage(cb) or decorator form
        PyRef cb = pos_or(a, 0);
        if (!cb) {
            return py_builtin("on_decorator",
                              [this, t = std::string(topic)](interpreter&,
                                                             const py_args& a2) {
                                  PyRef c2 =
                                      a2.pos.empty() ? nullptr : a2.pos[0];
                                  if (c2) {
                                      cb_box* b = keep_cb(i, c2);
                                      uint32_t token = 0;
                                      sao_plugins_ctx_subscribe(
                                          c(), t.c_str(), tr_event, b,
                                          &token);
                                  }
                                  return c2 ? c2 : py_none();
                              });
        }
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        sao_plugins_ctx_subscribe(c(), topic, tr_event, b, &token);
        return cb;
    }

    // ── timers ───────────────────────────────────────────────────────
    PyRef m_set_interval(interpreter&, const py_args& a) {
        PyRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("TypeError", "set_interval() missing callback", {});
        const double seconds =
            a.pos.size() > 1 ? kw_num(a, "seconds",
                                      [&] {
                                          bool ok = false;
                                          return py_to_float(a.pos[1], &ok);
                                      }())
                             : 0.0;
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        const int32_t r = sao_plugins_ctx_set_interval(
            c(), tr_timer, seconds, b, &token_raw);
        if (r != 0 || !token_raw)
            return py_none();
        PyRef out = py_str(token_raw);
        free_ctx_str(token_raw);
        return out;
    }
    PyRef m_set_timeout(interpreter&, const py_args& a) {
        PyRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("TypeError", "set_timeout() missing callback", {});
        const double seconds =
            a.pos.size() > 1 ? [&] {
                bool ok = false;
                return py_to_float(a.pos[1], &ok);
            }()
                             : kw_num(a, "seconds", 0.0);
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        const int32_t r = sao_plugins_ctx_set_timeout(
            c(), tr_timer, seconds, b, &token_raw);
        if (r != 0 || !token_raw)
            return py_none();
        PyRef out = py_str(token_raw);
        free_ctx_str(token_raw);
        return out;
    }
    PyRef m_clear_timer(interpreter&, const py_args& a) {
        const std::string tok = pos_str(i, a, 0);
        if (!tok.empty())
            sao_plugins_ctx_clear_timer(c(), tok.c_str());
        return py_none();
    }
    PyRef m_complete_timer(interpreter&, const py_args& a) {
        const std::string tok = pos_str(i, a, 0);
        if (!tok.empty())
            sao_plugins_ctx_complete_timer(c(), tok.c_str());
        return py_none();
    }
    PyRef m_run_on_ui(interpreter&, const py_args& a) {
        PyRef cb = pos_or(a, 0);
        if (!cb)
            return py_none();
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        sao_plugins_ctx_set_timeout(c(), tr_timer, 0.0, b, &token_raw);
        if (token_raw)
            free_ctx_str(token_raw);
        return py_none();
    }

    // ── notify / dialogs / windows ───────────────────────────────────
    PyRef m_notify(interpreter&, const py_args& a) {
        const std::string title = pos_str(i, a, 0);
        const std::string msg = pos_str(i, a, 1);
        const double dur =
            a.pos.size() > 2 ? [&] {
                bool ok = false;
                return py_to_float(a.pos[2], &ok);
            }()
                             : kw_num(a, "duration_s", 60.0);
        const std::string kind =
            a.pos.size() > 3 ? pos_str(i, a, 3)
                             : kw_str(a, "kind", "plugin");
        sao_plugins_ctx_notify(c(), title.c_str(), msg.c_str(), dur,
                               kind.c_str());
        return py_none();
    }
    PyRef m_dismiss_notify(interpreter&, const py_args&) {
        sao_plugins_ctx_dismiss_notify(c());
        return py_none();
    }
    PyRef m_toast(interpreter&, const py_args& a) {
        const std::string msg = pos_str(i, a, 0);
        sao_plugins_ctx_toast(c(), msg.c_str());
        return py_none();
    }
    PyRef m_open_file(interpreter&, const py_args& a) {
        std::string filters = "[]", title;
        if (!a.pos.empty()) {
            if (auto* s = as_str(a.pos[0]))
                filters = s->v;              // pipe-string form passes through
            else
                filters = jstr(i, a.pos[0]); // legacy [{"name","spec"}] json
        }
        title = pos_str(i, a, 1);
        wchar_t* path = nullptr;
        const int32_t r = sao_plugins_ctx_open_file(
            c(), filters.c_str(), title.c_str(), nullptr, 0, &path);
        if (r != 0 || !path)
            return py_none();
        PyRef out = py_str(narrow_w(path));
        loader::sao_plugins_ctx_free_wstring(path);
        return out;
    }
    PyRef m_open_window(interpreter&, const py_args& a) {
        const std::string pid = pos_str(i, a, 0);
        bool ok = false;
        const uint32_t w =
            a.pos.size() > 1 ? static_cast<uint32_t>(py_to_int(a.pos[1], &ok))
                             : 0;
        const uint32_t h =
            a.pos.size() > 2 ? static_cast<uint32_t>(py_to_int(a.pos[2], &ok))
                             : 0;
        const int32_t r = sao_plugins_ctx_open_window(
            c(), pid.empty() ? nullptr : pid.c_str(), w, h);
        return py_bool(r == 0);
    }

    // ── compositor family ────────────────────────────────────────────
    PyRef m_create_layer(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        bool ok = false;
        const uint32_t w =
            a.pos.size() > 1 ? static_cast<uint32_t>(py_to_int(a.pos[1], &ok))
                             : 0;
        const uint32_t h =
            a.pos.size() > 2 ? static_cast<uint32_t>(py_to_int(a.pos[2], &ok))
                             : 0;
        const int32_t x =
            a.pos.size() > 3 ? static_cast<int32_t>(py_to_int(a.pos[3], &ok))
                             : 0;
        const int32_t y =
            a.pos.size() > 4 ? static_cast<int32_t>(py_to_int(a.pos[4], &ok))
                             : 0;
        const int32_t z =
            a.pos.size() > 5 ? static_cast<int32_t>(py_to_int(a.pos[5], &ok))
                             : 140;
        const bool click_through = kw_bool(i, a, "click_through", true);
        const bool high_fps = kw_bool(i, a, "high_fps", false);
        const uint32_t target_fps =
            static_cast<uint32_t>(kw_num(a, "target_fps", 0));
        const int32_t r = sao_plugins_ctx_create_compositor_layer(
            c(), name.c_str(), w, h, x, y, z, click_through, high_fps,
            target_fps);
        return py_bool(r == 0);
    }
    PyRef m_upload_frame(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        std::string bytes;
        if (a.pos.size() > 1) {
            if (auto* s = as_str(a.pos[1]))
                bytes = s->v;
            else if (auto* b = as_bytes(a.pos[1]))
                bytes = b->v;
            else if (auto* l = as_list(a.pos[1])) {
                bytes.reserve(l->v.size());
                for (const auto& x : l->v) {
                    bool ok = false;
                    bytes += static_cast<char>(py_to_int(x, &ok));
                }
            }
        }
        bool ok = false;
        const uint32_t w =
            a.pos.size() > 2 ? static_cast<uint32_t>(py_to_int(a.pos[2], &ok))
                             : 0;
        const uint32_t h =
            a.pos.size() > 3 ? static_cast<uint32_t>(py_to_int(a.pos[3], &ok))
                             : 0;
        const int32_t r = sao_plugins_ctx_upload_compositor_frame(
            c(), name.c_str(),
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), w,
            h);
        if (r == 0 && a.pos.size() > 4 && !py_is_none(a.pos[4])) {
            const int32_t x = static_cast<int32_t>(py_to_int(a.pos[4], &ok));
            const int32_t y = static_cast<int32_t>(py_to_int(a.pos[5], &ok));
            sao_plugins_ctx_set_compositor_layer_position(c(), name.c_str(),
                                                          x, y);
        }
        return py_bool(r == 0);
    }
    PyRef m_layer_pos(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        bool ok = false;
        const int32_t x =
            a.pos.size() > 1 ? static_cast<int32_t>(py_to_int(a.pos[1], &ok))
                             : 0;
        const int32_t y =
            a.pos.size() > 2 ? static_cast<int32_t>(py_to_int(a.pos[2], &ok))
                             : 0;
        return py_bool(sao_plugins_ctx_set_compositor_layer_position(
                           c(), name.c_str(), x, y) == 0);
    }
    PyRef m_layer_visible(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        const bool v = a.pos.size() > 1 && i.truthy(a.pos[1]);
        return py_bool(sao_plugins_ctx_set_compositor_layer_visible(
                           c(), name.c_str(), v) == 0);
    }
    PyRef m_layer_destroy(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        return py_bool(sao_plugins_ctx_destroy_compositor_layer(
                           c(), name.c_str()) == 0);
    }
    PyRef m_layer_input(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        // kwargs: cursor_pos=, mouse_button=, cursor_leave=, scroll=
        // one shared user_data — a carrier dict holding the 4 callables.
        loader::compositor_cursor_pos_fn cp = nullptr;
        loader::compositor_mouse_button_fn mb = nullptr;
        loader::compositor_cursor_leave_fn cl = nullptr;
        loader::compositor_scroll_fn sc = nullptr;
        cb_box* carrier = keep_cb(i, py_dict());
        auto* cd = as_dict(carrier->fn);
        if (PyRef v = kw_get(a, "cursor_pos")) {
            cp = [](float x, float y, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                gil_guard g(*b->i);
                auto* d = as_dict(b->fn);
                try {
                    b->i->call(dict_get(d, py_str("cursor_pos")),
                               py_args{{py_float(x), py_float(y)}}, {});
                } catch (...) {
                }
            };
            dict_set(cd, py_str("cursor_pos"), v);
        }
        if (PyRef v = kw_get(a, "mouse_button")) {
            mb = [](uint32_t btn, bool pressed, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                gil_guard g(*b->i);
                auto* d = as_dict(b->fn);
                try {
                    b->i->call(dict_get(d, py_str("mouse_button")),
                               py_args{{py_int(btn), py_bool(pressed)}}, {});
                } catch (...) {
                }
            };
            dict_set(cd, py_str("mouse_button"), v);
        }
        if (PyRef v = kw_get(a, "cursor_leave")) {
            cl = [](void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                gil_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn),
                                         py_str("cursor_leave")),
                                {});
                } catch (...) {
                }
            };
            dict_set(cd, py_str("cursor_leave"), v);
        }
        if (PyRef v = kw_get(a, "scroll")) {
            sc = [](float dx, float dy, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                gil_guard g(*b->i);
                try {
                    b->i->call(dict_get(as_dict(b->fn), py_str("scroll")),
                               py_args{{py_float(dx), py_float(dy)}}, {});
                } catch (...) {
                }
            };
            dict_set(cd, py_str("scroll"), v);
        }
        const int32_t r = sao_plugins_ctx_set_compositor_layer_input(
            c(), name.c_str(), cp, mb, cl, sc, carrier);
        return py_bool(r == 0);
    }
    PyRef m_gpu_interop(interpreter&, const py_args& a) {
        mini_named_args(i, a, {}, 0);
        bool available = false;
        mini_compositor_status(i, sao_plugins_ctx_compositor_gpu_interop_available(c(), &available));
        return py_bool(available);
    }
    PyRef m_layer_shared_tex(interpreter&, const py_args& a) {
        const auto args = mini_named_args(i, a, {"name", "handle", "width", "height"}, 2);
        const auto name = mini_menu_text(i, args[0], true);
        const auto handle = mini_texture_uint(i, args[1], (std::numeric_limits<uint64_t>::max)(), true);
        const auto width = mini_texture_uint(i, args[2], (std::numeric_limits<uint32_t>::max)());
        const auto height = mini_texture_uint(i, args[3], (std::numeric_limits<uint32_t>::max)());
        if ((handle && (!width || !height)) || (!handle && (width || height)))
            i.raise_exc("ValueError", "texture source needs positive dimensions; clear uses handle=width=height=0", {});
        mini_compositor_status(i, sao_plugins_ctx_set_compositor_layer_shared_texture_source(
            c(), name.c_str(), handle, static_cast<uint32_t>(width), static_cast<uint32_t>(height)));
        return py_true();
    }
    PyRef m_layer_mmf(interpreter&, const py_args& a) {
        const auto args = mini_named_args(i, a, {"name", "mmf"}, 2);
        const auto name = mini_menu_text(i, args[0], true);
        if (!as_str(args[1])) i.raise_exc("TypeError", "MMF name must be a string", {});
        const auto mmf = mini_menu_text(i, args[1]);
        mini_compositor_status(i, sao_plugins_ctx_set_compositor_layer_mmf_source(c(), name.c_str(), mmf.c_str()));
        return py_true();
    }
    PyRef m_layer_refresh(interpreter&, const py_args&) {
        return py_float(0.0);
    }
    PyRef m_layer_shared_tex_active(interpreter&, const py_args& a) {
        const auto args = mini_named_args(i, a, {"name"}, 1);
        const auto name = mini_menu_text(i, args[0], true);
        bool active = false;
        mini_compositor_status(i, sao_plugins_ctx_compositor_layer_shared_texture_active(c(), name.c_str(), &active));
        return py_bool(active);
    }

    // ── registrations ────────────────────────────────────────────────
    PyRef m_register_hotkey(interpreter&, const py_args& a) {
        const std::string id = pos_str(i, a, 0);
        PyRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("TypeError", "register_hotkey() missing callback", {});
        const std::string defkey =
            a.pos.size() > 2 ? pos_str(i, a, 2) : kw_str(a, "default_key");
        const std::string label =
            a.pos.size() > 3 ? pos_str(i, a, 3) : kw_str(a, "label");
        cb_box* b = keep_cb(i, cb);
        const int32_t r = sao_plugins_ctx_register_hotkey(
            c(), id.c_str(), defkey.c_str(), label.c_str(), tr_hotkey, b);
        return py_bool(r == 0);
    }
    PyRef m_unregister_hotkey(interpreter&, const py_args& a) {
        sao_plugins_ctx_unregister_hotkey(c(), pos_str(i, a, 0).c_str());
        return py_none();
    }
    PyRef m_render_hook(interpreter&, const py_args& a) {
        const std::string surface = pos_str(i, a, 0);
        PyRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("TypeError",
                        "register_render_hook() missing callback", {});
        const float prio =
            a.pos.size() > 2 ? [&] {
                bool ok = false;
                return static_cast<float>(py_to_float(a.pos[2], &ok));
            }()
                             : static_cast<float>(kw_num(a, "priority", 0.0));
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_register_render_hook(
            c(), surface.c_str(), prio, tr_render_hook, b, &token);
        if (r != 0)
            return py_none();
        return py_str(fmt_token("hook", token));
    }
    PyRef m_unregister_render_hook(interpreter&, const py_args& a) {
        uint32_t t = 0;
        if (parse_token(pos_str(i, a, 0), &t))
            sao_plugins_ctx_unregister_render_hook(c(), t);
        return py_none();
    }
    PyRef m_set_overlay(interpreter&, const py_args& a) {
        const std::string surface = pos_str(i, a, 0);
        const std::string spec =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("{}");
        return py_bool(sao_plugins_ctx_set_overlay(c(), surface.c_str(),
                                                   spec.c_str()) == 0);
    }
    PyRef m_clear_overlay(interpreter&, const py_args& a) {
        const std::string surface = pos_str(i, a, 0);
        return py_bool(sao_plugins_ctx_clear_overlay(
                           c(), surface.empty() ? nullptr
                                                : surface.c_str()) == 0);
    }
    PyRef m_request_redraw(interpreter&, const py_args& a) {
        const std::string surface = pos_str(i, a, 0);
        const std::string reason = pos_str(i, a, 1);
        sao_plugins_ctx_request_redraw(c(), surface.c_str(),
                                       reason.empty() ? nullptr
                                                      : reason.c_str());
        return py_none();
    }

    PyRef m_register_ui_panel(interpreter&, const py_args& a) {
        const std::string id = pos_str(i, a, 0);
        PyRef meta = pos_or(a, 1, py_dict());
        PyRef render = kw_get(a, "render");
        PyRef on_action = kw_get(a, "on_action");
        // shared user_data for render + on_action → carrier dict keyed
        cb_box* carrier = keep_cb(i, py_dict(), id);
        auto* cd = as_dict(carrier->fn);
        if (render)
            dict_set(cd, py_str("render"), render);
        if (on_action)
            dict_set(cd, py_str("on_action"), on_action);
        // render callback reads cd["render"]; action reads cd["on_action"]
        const int32_t r = sao_plugins_ctx_register_ui_panel(
            c(), id.c_str(), jstr(i, meta).c_str(),
            render ? [](const char* payload, char** out,
                        void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i || !out)
                    return -1;
                *out = nullptr;
                gil_guard g(*b->i);
                try {
                    PyRef fn =
                        dict_get(as_dict(b->fn), py_str("render"));
                    PyRef r2 =
                        b->i->call(fn,
                                   py_args{{parse_json(*b->i, payload)}}, {});
                    const std::string s =
                        r2 ? jstr(*b->i, r2) : std::string("{}");
                    *out = static_cast<char*>(std::malloc(s.size() + 1));
                    if (!*out)
                        return -2;
                    std::memcpy(*out, s.c_str(), s.size() + 1);
                    return 0;
                } catch (...) {
                    return -1;
                }
            }
                        : static_cast<loader::render_callback_fn>(nullptr),
            on_action ? [](const char* action_id, const char* payload,
                           char** out, void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i || !out)
                    return -1;
                *out = nullptr;
                gil_guard g(*b->i);
                try {
                    PyRef fn = dict_get(as_dict(b->fn),
                                        py_str("on_action"));
                    PyRef r2 = b->i->call(
                        fn,
                        py_args{{py_str(action_id ? action_id : ""),
                                 parse_json(*b->i, payload)}},
                        {});
                    const std::string s =
                        r2 ? jstr(*b->i, r2) : std::string("{}");
                    *out = static_cast<char*>(std::malloc(s.size() + 1));
                    if (!*out)
                        return -2;
                    std::memcpy(*out, s.c_str(), s.size() + 1);
                    return 0;
                } catch (...) {
                    return -1;
                }
            }
                      : static_cast<loader::action_callback_fn>(nullptr),
            carrier);
        if (r == 0) {
            // ledger entry (pyhost parity)
            auto* panels =
                as_dict(dict_get(as_dict(ledgers), py_str("panels")));
            if (panels) {
                auto rec = py_dict();
                dict_set(as_dict(rec), py_str("id"), py_str(id));
                dict_set(as_dict(rec), py_str("meta"), meta);
                dict_set(as_dict(rec), py_str("render"),
                         render ? render : py_none());
                dict_set(as_dict(rec), py_str("on_action"),
                         on_action ? on_action : py_none());
                dict_set(panels, py_str(id), rec);
            }
        }
        return py_bool(r == 0);
    }
    PyRef m_register_extension(interpreter&, const py_args& a,
                               const char* kind) {
        const std::string id = pos_str(i, a, 0);
        PyRef meta = pos_or(a, 1, py_dict());
        PyRef handler = a.pos.size() > 2 ? a.pos[2] : kw_get(a, "handler");
        cb_box* hb = handler ? keep_cb(i, handler) : nullptr;
        const int32_t r = sao_plugins_ctx_register_extension(
            c(), kind, id.c_str(), jstr(i, meta).c_str(),
            hb ? reinterpret_cast<void*>(tr_action) : nullptr, hb);
        return py_bool(r == 0);
    }
    PyRef m_menu_category(interpreter&, const py_args& a) {
        const auto args = mini_named_args(i, a, {"name", "icon", "builder", "priority"}, 3);
        auto menu = std::make_shared<mini_menu>();
        menu->name = mini_menu_text(i, args[0], true);
        menu->icon = mini_menu_text(i, args[1]);
        if (!mini_menu_callable(i, args[2])) i.raise_exc("TypeError", "menu builder must be callable", {});
        if (args[3]) {
            bool ok = false;
            menu->priority = py_to_float(args[3], &ok);
            if (!ok || !std::isfinite(menu->priority)) i.raise_exc("ValueError", "menu priority must be finite", {});
        }
        auto* context = c();
        menu->provider_id = "menu-" + mini_menu_hash(menu->name);
        menu->root_id = "plugin:" + mini_menu_hash(i.cfg.plugin_id + "\n" + menu->name);
        cb_box* box = keep_cb(i, args[2]);
        box->menu = menu;
        loader::entity_root_contribution_descriptor root{sizeof(root), menu->provider_id.c_str(),
            menu->root_id.c_str(), menu->name.c_str(), menu->icon.c_str(), menu->priority};
        loader::context_entity_provider_descriptor_v3 provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = menu->provider_id.c_str();
        provider.snapshot = tr_menu_snapshot;
        provider.user_data = box;
        provider.root_contribution = &root;
        provider.action_handler_v2 = tr_menu_action;
        provider.action_user_data = box;
        const int32_t status = sao_plugins_ctx_register_entity_provider_v3(context, &provider);
        if (status != SAO_OK) {
            {
                std::lock_guard lock(g_cb_mu);
                auto& boxes = g_cb_keep[&i];
                const auto found = std::find_if(boxes.begin(), boxes.end(), [&](const cb_ptr& value) { return value.get() == box; });
                if (found != boxes.end()) boxes.erase(found);
            }
            i.raise_exc("RuntimeError", "menu registration failed: " + std::to_string(status), {});
        }
        return py_true();
    }
    PyRef m_menu_surface(interpreter&, const py_args& a) {
        const std::string sid = pos_str(i, a, 0);
        PyRef desc = pos_or(a, 1, py_dict());
        const float prio =
            a.pos.size() > 2 ? [&] {
                bool ok = false;
                return static_cast<float>(py_to_float(a.pos[2], &ok));
            }()
                             : 0.0f;
        const int32_t r = sao_plugins_ctx_register_menu_surface(
            c(), sid.c_str(), jstr(i, desc).c_str(), prio);
        return py_bool(r == 0);
    }
    PyRef m_register_action_handler(interpreter&, const py_args& a) {
        PyRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("TypeError",
                        "register_action_handler() missing handler", {});
        cb_box* b = keep_cb(i, cb);
        // action handlers register as an extension of kind "action_handler"
        const int32_t r = sao_plugins_ctx_register_extension(
            c(), "action_handler", "default", "{}",
            reinterpret_cast<void*>(tr_action), b);
        return py_bool(r == 0);
    }
    PyRef m_register_engine(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        PyRef eng = pos_or(a, 1);
        dict_set(as_dict(engines), py_str(name), eng ? eng : py_none());
        return py_none();
    }
    PyRef m_get_engine(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        if (PyRef v = dict_get(as_dict(engines), py_str(name)))
            return py_is_none(v) ? pos_or(a, 1, py_none()) : v;
        void* raw = sao_plugins_ctx_get_engine(c(), name.c_str());
        if (!raw)
            return pos_or(a, 1, py_none());
        // opaque native engine pointer — wrap minimally
        auto d = py_dict();
        dict_set(as_dict(d), py_str("__native_engine__"), py_true());
        dict_set(as_dict(d), py_str("name"), py_str(name));
        dict_set(as_dict(d), py_str("handle"),
                 py_int(reinterpret_cast<int64_t>(raw)));
        return d;
    }
    PyRef m_require_engine(interpreter&, const py_args& a) {
        PyRef v = m_get_engine(i, a);
        if (py_is_none(v))
            i.raise_exc("RuntimeError",
                        "required engine missing: " + pos_str(i, a, 0), {});
        return v;
    }
    PyRef m_call_engine(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        const std::string method = pos_str(i, a, 1);
        PyRef eng = m_get_engine(i, a);
        if (!py_is_none(eng) && !as_dict(eng)) {
            PyRef fn = i.getattr(eng, method);
            if (!fn)
                i.raise_exc("AttributeError",
                            "engine '" + name + "' has no '" + method + "'",
                            {});
            py_args ca;
            for (std::size_t k = 2; k < a.pos.size(); ++k)
                ca.pos.push_back(a.pos[k]);
            ca.kw = a.kw;
            return i.call(fn, ca, {});
        }
        return py_none();
    }
    PyRef m_call_runtime(interpreter&, const py_args&) {
        return py_none();    // v1 runtime RPC — graceful no-op subset
    }
    PyRef m_register_data_source(interpreter&, const py_args& a) {
        const std::string id = pos_str(i, a, 0);
        PyRef meta = pos_or(a, 1, py_dict());
        PyRef start = kw_get(a, "start");
        PyRef stop = kw_get(a, "stop");
        // one user_data for start+stop → carrier dict
        cb_box* carrier = keep_cb(i, py_dict(), id);
        auto* cd = as_dict(carrier->fn);
        if (start)
            dict_set(cd, py_str("start"), start);
        if (stop)
            dict_set(cd, py_str("stop"), stop);
        const int32_t r = sao_plugins_ctx_register_data_source(
            c(), id.c_str(), jstr(i, meta).c_str(),
            start ? [](void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return 0;
                gil_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn), py_str("start")),
                                {});
                } catch (...) {
                    return -1;
                }
                return 0;
            }
                  : static_cast<loader::data_source_start_fn>(nullptr),
            stop ? [](void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return 0;
                gil_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn), py_str("stop")),
                                {});
                } catch (...) {
                    return -1;
                }
                return 0;
            }
                 : static_cast<loader::data_source_stop_fn>(nullptr),
            carrier);
        return py_bool(r == 0);
    }
    PyRef m_register_thread(interpreter&, const py_args&) {
        return py_none();    // bookkeeping-only in v1
    }
    PyRef m_ensure_requirements(interpreter&, const py_args& a) {
        const bool install =
            a.pos.empty() ? true : i.truthy(a.pos[0]);
        char* raw = nullptr;
        if (sao_plugins_ctx_ensure_requirements(c(), install ? true : false,
                                              &raw) != 0 ||
            !raw)
            return py_dict();
        PyRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    PyRef m_load_local(interpreter&, const py_args& a) {
        const std::string rel = pos_str(i, a, 0);
        script::load_local_result kind{};
        std::shared_ptr<script::script_module> mod;
        std::wstring abs;
        std::string diag;
        const int32_t r = script::runtime_bridge_load_local(
            c(), i.cfg.plugin_id.c_str(),
            i.cfg.plugin_root.c_str(), rel.c_str(), &kind, &mod, &abs,
            &diag);
        if (r != 0)
            return py_none();
        switch (kind) {
        case script::load_local_result::module:
            if (mod)
                return wrap_script_module(i, mod);
            return py_none();
        case script::load_local_result::path_only:
            return py_str(narrow_w(abs));
        default:
            if (i.on_log)
                i.on_log("load_local(" + rel + "): " + diag);
            return py_none();
        }
    }
    PyRef m_open(interpreter&, const py_args& a) {
        const auto* p = as_str(pos_or(a, 0));
        if (!p)
            i.raise_exc("TypeError", "open() path must be str", {});
        const std::string mode =
            a.pos.size() > 1 ? pos_str(i, a, 1, "r") : "r";
        return pymini_open_file(i, p->v, mode);
    }
    PyRef m_time(interpreter&, const py_args&) {
        return py_float(static_cast<double>(
                            std::chrono::system_clock::now()
                                .time_since_epoch()
                                .count()) /
                        static_cast<double>(
                            std::chrono::system_clock::period::den) *
                        static_cast<double>(
                            std::chrono::system_clock::period::num));
    }

    // ── ui.* spec builder ────────────────────────────────────────────
    PyRef make_ui() {
        auto d = py_dict();
        std::size_t n_methods = 0;
        const char* const* names = script::script_ui_methods(&n_methods);
        for (std::size_t k = 0; k < n_methods; ++k) {
            const std::string mname = names[k];
            dict_set(as_dict(d), py_str(mname),
                     py_builtin("ui." + mname,
                                [this, mname](interpreter&,
                                              const py_args& a) {
                                    nlohmann::json req;
                                    if (a.kw.empty()) {
                                        req = nlohmann::json::array();
                                        for (const auto& p : a.pos)
                                            req.push_back(
                                                json_from_py(i, p));
                                    } else {
                                        req = nlohmann::json::object();
                                        for (const auto& [k2, v] : a.kw)
                                            req[k2] = json_from_py(i, v);
                                        for (std::size_t pi = 0;
                                             pi < a.pos.size(); ++pi)
                                            req[std::to_string(pi)] =
                                                json_from_py(i, a.pos[pi]);
                                    }
                                    nlohmann::json node;
                                    std::string err;
                                    if (!script::script_ui_build(
                                            mname.c_str(), req, node, err))
                                        i.raise_exc("ValueError", err, {});
                                    return py_from_json(i, node);
                                }));
        }
        // ui.canvas compat: dict spec passthrough is already handled by the
        // generic build path (canvas op nodes are dict-shaped).
        return d;
    }

    // engine/property sub-objects
    PyRef make_event_bus() {
        auto d = py_dict();
        dict_set(as_dict(d), py_str("subscribe"),
                 method("subscribe",
                        [this](interpreter&, const py_args& a) {
                            return m_subscribe(i, a);
                        }));
        dict_set(as_dict(d), py_str("subscribe_once"),
                 method("subscribe_once",
                        [this](interpreter&, const py_args& a) {
                            return m_subscribe_once(i, a);
                        }));
        dict_set(as_dict(d), py_str("unsubscribe"),
                 method("unsubscribe",
                        [this](interpreter&, const py_args& a) {
                            return m_unsubscribe(i, a);
                        }));
        dict_set(as_dict(d), py_str("emit"),
                 method("emit",
                        [this](interpreter&, const py_args& a) {
                            return m_emit(i, a);
                        }));
        dict_set(as_dict(d), py_str("on"),
                 method("on",
                        [this](interpreter&, const py_args& a) {
                            return m_on(i, a);
                        }));
        return d;
    }
    PyRef make_mem() {
        // ctx.mem — the reflective engine surface's `mem.*` group exposed
        // with the group prefix stripped: catalog "mem.read_u64" binds as
        // ctx.mem.read_u64(...).  This mirrors the v1 MemAccess facade: the
        // accessor object exists unconditionally; an actual call surfaces
        // the provider status (missing mem provider → RuntimeError via the
        // engine-call envelope).
        auto d = py_dict();
        const std::size_t catalog_n = sdk_binding::sdk_engine_catalog_size();
        for (std::size_t k = 0; k < catalog_n; ++k) {
            const sdk_binding::sdk_engine_function_desc* desc =
                sdk_binding::sdk_engine_catalog_at(k);
            if (desc == nullptr || desc->name == nullptr)
                continue;
            const std::string_view full(desc->name);
            if (full.size() <= 4 || full.substr(0, 4) != "mem.")
                continue;
            const std::string attr(full.substr(4));
            if (attr.empty() || dict_get(as_dict(d), py_str(attr)))
                continue;
            dict_set(as_dict(d), py_str(attr),
                     py_builtin("mem." + attr,
                                [this, desc](interpreter&,
                                             const py_args& a2) {
                                    return engine_named_call(desc, a2);
                                }));
        }
        // module proxy so `ctx.mem.read_u64` attribute access works — plain
        // dicts are not attribute-mapped in pymini.
        auto proxy = std::make_shared<PyModuleProxyObj>();
        proxy->name = "ctx.mem";
        proxy->dict = d;
        return proxy;
    }
    // ── reflective engine surface (binding_engine.h) ───────────────
    // Lazily bind a per-plugin SaoSdkContext and route every engine call
    // through sao_plugins_sdk_context_dispatch.  The result envelope is
    // {"status","result"}; a non-zero status raises RuntimeError.
    const SaoSdkContext* engine_ctx() {
        if (!engine_sdk_tried) {
            engine_sdk_tried = true;
            SaoSdkContext* created = nullptr;
            sao_sdk_status_t st = sao_sdk_context_create(
                root_utf8.empty() ? nullptr : root_utf8.c_str(),
                i.cfg.plugin_id.empty() ? nullptr : i.cfg.plugin_id.c_str(),
                &created);
            if (st == SAO_SDK_OK && created != nullptr)
                st = sao_sdk_context_bind_platform_services(created);
            if (st == SAO_SDK_OK && created != nullptr) {
                engine_sdk = created;
            } else if (created != nullptr) {
                (void)sao_sdk_context_try_destroy(created);
            }
            engine_sdk_status = static_cast<int32_t>(st);
        }
        if (engine_sdk == nullptr)
            i.raise_exc("RuntimeError",
                        "engine context unavailable (status " +
                            std::to_string(engine_sdk_status) + ")",
                        {});
        return engine_sdk;
    }

    // Core dispatch.  args_text is the serialized request arguments (the
    // {"name","args"} engine envelope for method_engine_call; "{}" for
    // method_engine_list).  Once engine.on() has armed a channel the
    // generic engine_callback trampoline rides on every request so
    // callback-capable entries can deliver (channel, payload) to script.
    PyRef engine_dispatch_call(sdk_binding::sdk_method_id method,
                               const std::string& args_text,
                               const std::string& err_subject) {
        const SaoSdkContext* sdk = engine_ctx();
        std::string buf(static_cast<std::size_t>(256) * 1024U, '\0');
        int32_t st = SAO_OK;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::size_t required = 0;
            sdk_binding::sdk_context_call_request request{};
            request.args_json_utf8 = args_text.data();
            request.args_size = args_text.size();
            if (engine_cb_hub != nullptr) {
                request.engine_callback = &tr_engine_channel;
                request.callback_user_data = engine_cb_hub;
            }
            request.out_result_json_utf8 = buf.data();
            request.out_capacity = buf.size();
            request.out_required = &required;
            st = sdk_binding::sao_plugins_sdk_context_dispatch(sdk, method,
                                                             &request);
            if (st == SAO_ERR_BUFFER_TOO_SMALL && attempt == 0 &&
                required > buf.size() &&
                required <= sdk_binding::kMaximumBindingJsonBytes + 1U) {
                buf.assign(required, '\0');
                continue;
            }
            break;
        }
        if (st != SAO_OK)
            i.raise_exc("RuntimeError",
                        "engine call " + err_subject +
                            " failed: " + std::to_string(st),
                        {});
        nlohmann::json env;
        int64_t inner = 0;
        const nlohmann::json* result = nullptr;
        try {
            env = nlohmann::json::parse(buf);
            if (env.is_object()) {
                const auto st_it = env.find("status");
                if (st_it != env.end() && st_it->is_number())
                    inner = st_it->get<int64_t>();
                const auto r_it = env.find("result");
                if (r_it != env.end())
                    result = &*r_it;
            }
        } catch (...) {
        }
        if (inner != 0)
            i.raise_exc("RuntimeError",
                        "engine call " + err_subject +
                            " failed: " + std::to_string(inner),
                        {});
        return result != nullptr ? py_from_json(i, *result) : py_none();
    }

    // ctx.engine_call(name, args_dict=None, *, callback_channel=...) — raw
    // engine envelope passthrough.
    PyRef m_engine_call(interpreter&, const py_args& a) {
        const std::string name = pos_str(i, a, 0);
        if (name.empty())
            i.raise_exc("TypeError",
                        "engine_call(name, args) requires a name", {});
        nlohmann::json args = nlohmann::json::object();
        if (PyRef ad = pos_or(a, 1)) {
            if (!py_is_none(ad)) {
                args = json_from_py(i, ad);
                if (!args.is_object())
                    i.raise_exc("TypeError",
                                "engine_call() args must be a dict", {});
            }
        }
        nlohmann::json envelope = nlohmann::json::object();
        envelope["name"] = name;
        envelope["args"] = std::move(args);
        if (PyRef ch = kw_get(a, "callback_channel"))
            envelope["callback_channel"] = py_to_str(i, ch);
        return engine_dispatch_call(
            sdk_binding::sdk_method_id::method_engine_call, envelope.dump(),
            name);
    }
    // ctx.engine.list() — runtime catalog via method_engine_list →
    // [{name,args,available}].
    PyRef m_engine_list(interpreter&, const py_args&) {
        return engine_dispatch_call(
            sdk_binding::sdk_method_id::method_engine_list, std::string("{}"),
            "engine.list");
    }
    // ctx.engine.on(channel, cb) — registers cb on a generic
    // engine_callback channel (e.g. "net.frame", "ui.render_hook").
    // Re-registering a channel replaces the previous callable.
    PyRef m_engine_on(interpreter&, const py_args& a) {
        const std::string channel = [&] {
            const std::string s = pos_str(i, a, 0);
            return s.empty() ? kw_str(a, "channel") : s;
        }();
        PyRef cb = pos_or(a, 1);
        if (!cb)
            cb = kw_get(a, "callback");
        if (channel.empty())
            i.raise_exc("TypeError", "engine.on() requires a channel", {});
        if (!cb)
            i.raise_exc("TypeError", "engine.on() missing callback", {});
        if (engine_cb_hub == nullptr)
            engine_cb_hub = keep_cb(i, py_dict(), "engine_channels");
        dict_set(as_dict(engine_cb_hub->fn), py_str(channel), cb);
        return cb;
    }
    // One callable per catalog entry: positionals map to desc->arg_names
    // order, kwargs overlay them; positionals beyond arg_count pass through
    // under index keys (engine-side validation reports the real arg names).
    PyRef engine_named_call(
        const sdk_binding::sdk_engine_function_desc* desc, const py_args& a) {
        nlohmann::json args = nlohmann::json::object();
        for (std::size_t k = 0; k < a.pos.size(); ++k) {
            const char* named =
                desc != nullptr && desc->arg_names != nullptr &&
                        k < desc->arg_count
                    ? desc->arg_names[k]
                    : nullptr;
            if (named != nullptr)
                args[named] = json_from_py(i, a.pos[k]);
            else
                args[std::to_string(k)] = json_from_py(i, a.pos[k]);
        }
        for (const auto& [k, v] : a.kw)
            args[k] = json_from_py(i, v);
        const std::string name =
            desc != nullptr && desc->name != nullptr ? desc->name : "";
        nlohmann::json envelope = nlohmann::json::object();
        envelope["name"] = name;
        envelope["args"] = std::move(args);
        return engine_dispatch_call(
            sdk_binding::sdk_method_id::method_engine_call, envelope.dump(),
            name);
    }

    PyRef make_engine_obj() {
        auto d = py_dict();
        dict_set(as_dict(d), py_str("get"),
                 method("engine.get",
                        [this](interpreter&, const py_args& a) {
                            return m_get_engine(i, a);
                        }));
        dict_set(as_dict(d), py_str("call"),
                 method("engine.call",
                        [this](interpreter&, const py_args& a) {
                            return m_call_engine(i, a);
                        }));
        dict_set(as_dict(d), py_str("require"),
                 method("engine.require",
                        [this](interpreter&, const py_args& a) {
                            return m_require_engine(i, a);
                        }));
        dict_set(as_dict(d), py_str("register"),
                 method("engine.register",
                        [this](interpreter&, const py_args& a) {
                            return m_register_engine(i, a);
                        }));
        // Reflective engine surface (binding_engine.h): engine.list() +
        // engine.on() plus one callable per catalog entry named by its
        // catalog name with '.'→'_' ("mem.read_u64" → mem_read_u64).
        // Catalog names always carry a group prefix, so they cannot collide
        // with the get/call/require/register methods above.
        dict_set(as_dict(d), py_str("list"),
                 method("engine.list",
                        [this](interpreter&, const py_args& a) {
                            return m_engine_list(i, a);
                        }));
        dict_set(as_dict(d), py_str("on"),
                 method("engine.on",
                        [this](interpreter&, const py_args& a) {
                            return m_engine_on(i, a);
                        }));
        const std::size_t catalog_n = sdk_binding::sdk_engine_catalog_size();
        for (std::size_t k = 0; k < catalog_n; ++k) {
            const sdk_binding::sdk_engine_function_desc* desc =
                sdk_binding::sdk_engine_catalog_at(k);
            if (desc == nullptr || desc->name == nullptr)
                continue;
            std::string attr(desc->name);
            for (char& ch : attr)
                if (ch == '.')
                    ch = '_';
            if (attr.empty() || dict_get(as_dict(d), py_str(attr)))
                continue;
            dict_set(as_dict(d), py_str(attr),
                     py_builtin("engine." + attr,
                                [this, desc](interpreter&,
                                             const py_args& a2) {
                                    return engine_named_call(desc, a2);
                                }));
        }
        // wrap as module proxy so `ctx.engine.<fn>` attribute access works —
        // plain dicts are not attribute-mapped in pymini.
        auto proxy = std::make_shared<PyModuleProxyObj>();
        proxy->name = "ctx.engine";
        proxy->dict = d;
        return proxy;
    }

    std::string narrow_w(const std::wstring& w) {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                          static_cast<int>(w.size()), nullptr,
                                          0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()), s.data(), n, nullptr,
                            nullptr);
        return s;
    }

    // script_module → module_proxy
    PyRef wrap_script_module(interpreter&, const std::shared_ptr<script::script_module>& mod) {
        auto proxy = std::make_shared<PyModuleProxyObj>();
        proxy->name = mod->module_id();
        proxy->dict = py_dict();
        for (const auto& mname : mod->member_names()) {
            script::script_value_ptr v;
            std::string err;
            if (mod->get(mname, &v, &err) == 0 && v &&
                v->k == script::script_value::kind::function) {
                dict_set(as_dict(proxy->dict), py_str(mname),
                         py_builtin(mname,
                                    [this, mod, mname](interpreter& i2,
                                                       const py_args& a2) {
                                        std::vector<script::script_value_ptr>
                                            args;
                                        for (const auto& p : a2.pos)
                                            args.push_back(py_to_sv(i2, p));
                                        script::script_value_ptr out;
                                        std::string err2;
                                        if (mod->call(mname, args, &out,
                                                      &err2) != 0)
                                            i2.raise_exc("RuntimeError",
                                                         "load_local module call failed: " +
                                                             mname + ": " + err2,
                                                         {});
                                        return sv_to_py(i2, out);
                                    }));
            } else {
                // data member — marshal eagerly
                dict_set(as_dict(proxy->dict), py_str(mname),
                         sv_to_py(i, v));
            }
        }
        return proxy;
    }

    // script_value ↔ PyRef marshalling (cross-language boundary)
    script::script_value_ptr py_to_sv(interpreter&, const PyRef& v) {
        if (!v)
            return script::script_value::null_value();
        switch (v->kind) {
        case py_kind::none_:
            return script::script_value::null_value();
        case py_kind::boolean:
            return script::script_value::make_boolean(as_bool(v)->v);
        case py_kind::integer:
            return script::script_value::make_integer(as_int(v)->v);
        case py_kind::number:
            return script::script_value::make_number(as_float(v)->v);
        case py_kind::string:
            return script::script_value::make_string(as_str(v)->v);
        case py_kind::bytes_:
            return script::script_value::make_bytes(as_bytes(v)->v);
        case py_kind::list:
        case py_kind::tuple_: {
            std::vector<script::script_value_ptr> items;
            const auto& src =
                v->kind == py_kind::list ? as_list(v)->v : as_tuple(v)->v;
            for (const auto& x : src)
                items.push_back(py_to_sv(i, x));
            return script::script_value::make_list(std::move(items));
        }
        case py_kind::dict: {
            std::vector<std::pair<std::string, script::script_value_ptr>>
                obj;
            for (const auto& [k, x] : as_dict(v)->items)
                obj.emplace_back(py_to_str(i, k), py_to_sv(i, x));
            return script::script_value::make_map(std::move(obj));
        }
        default:
            return script::script_value::make_string(py_to_str(i, v));
        }
    }
    PyRef sv_to_py(interpreter&, const script::script_value_ptr& v) {
        if (!v)
            return py_none();
        using k = script::script_value::kind;
        switch (v->k) {
        case k::null:
            return py_none();
        case k::boolean:
            return py_bool(v->boolean);
        case k::integer:
            return py_int(v->integer);
        case k::number:
            return py_float(v->number);
        case k::string:
            return py_str(v->text);
        case k::bytes:
            return py_bytes(v->text);
        case k::list: {
            std::vector<PyRef> items;
            for (const auto& x : v->items)
                items.push_back(sv_to_py(i, x));
            return py_list(std::move(items));
        }
        case k::map: {
            auto d = py_dict();
            for (const auto& [k2, x] : v->object)
                dict_set(as_dict(d), py_str(k2), sv_to_py(i, x));
            return d;
        }
        case k::function:
            if (!v->call)
                return py_none();
            return py_builtin("cross_fn",
                              [v](interpreter& i2, const py_args& a2) {
                                  std::vector<script::script_value_ptr> args;
                                  ctx_builder* self = nullptr;      // unused
                                  (void)self;
                                  interpreter& ii = i2;
                                  for (const auto& p : a2.pos)
                                      args.push_back([&] {
                                          // local marshalling dup (no builder)
                                          if (!p || p->kind == py_kind::none_)
                                              return script::script_value::
                                                  null_value();
                                          switch (p->kind) {
                                          case py_kind::boolean:
                                              return script::script_value::
                                                  make_boolean(
                                                      as_bool(p)->v);
                                          case py_kind::integer:
                                              return script::script_value::
                                                  make_integer(
                                                      as_int(p)->v);
                                          case py_kind::number:
                                              return script::script_value::
                                                  make_number(
                                                      as_float(p)->v);
                                          case py_kind::string:
                                              return script::script_value::
                                                  make_string(as_str(p)->v);
                                          case py_kind::bytes_:
                                              return script::script_value::
                                                  make_bytes(as_bytes(p)->v);
                                          default:
                                              return script::script_value::
                                                  make_string(
                                                      py_to_str(ii, p));
                                          }
                                      }());
                                  script::script_value_ptr out;
                                  std::string err;
                                  if (v->call(args, &out, &err) != 0)
                                      i2.raise_exc("RuntimeError",
                                                   "cross-language call failed: " +
                                                       err,
                                                   {});
                                  if (!out)
                                      return py_none();
                                  using kk = script::script_value::kind;
                                  switch (out->k) {
                                  case kk::boolean:
                                      return py_bool(out->boolean);
                                  case kk::integer:
                                      return py_int(out->integer);
                                  case kk::number:
                                      return py_float(out->number);
                                  case kk::string:
                                      return py_str(out->text);
                                  case kk::bytes:
                                      return py_bytes(out->text);
                                  default:
                                      return py_none();
                                  }
                              });
        default:
            return py_none();
        }
    }

    // ── install all methods into a module_proxy dict ─────────────────
    PyRef build() {
        auto proxy = std::make_shared<PyModuleProxyObj>();
        proxy->name = "ctx";
        proxy->dict = py_dict();
        auto* d = as_dict(proxy->dict);
        auto put = [&](const char* n, py_native_fn f) {
            dict_set(d, py_str(n), method(n, std::move(f)));
        };
        auto putv = [&](const char* n, PyRef v) {
            dict_set(d, py_str(n), std::move(v));
        };

        // properties
        putv("plugin_id", py_str(i.cfg.plugin_id));
        putv("path", py_str(root_utf8));
        putv("web_path", py_str(root_utf8 + "/web"));
        putv("assets_path", py_str(root_utf8 + "/assets"));
        putv("should_stop",
             py_builtin("should_stop",
                        [this](interpreter&, const py_args&) {
                            return py_bool(sao_plugins_ctx_should_stop(c()));
                        }));
        putv("owner", py_str(i.cfg.plugin_id));

        // events + props
        put("log", [this](interpreter&, const py_args& a) { return m_log(i, a); });
        put("subscribe", [this](interpreter&, const py_args& a) { return m_subscribe(i, a); });
        put("subscribe_once", [this](interpreter&, const py_args& a) { return m_subscribe_once(i, a); });
        put("unsubscribe", [this](interpreter&, const py_args& a) { return m_unsubscribe(i, a); });
        put("on", [this](interpreter&, const py_args& a) { return m_on(i, a); });
        put("on_damage", [this](interpreter&, const py_args& a) { return on_topic(i, a, "damage"); });
        put("on_heal", [this](interpreter&, const py_args& a) { return on_topic(i, a, "heal"); });
        put("on_skill", [this](interpreter&, const py_args& a) { return on_topic(i, a, "skill"); });
        put("on_boss", [this](interpreter&, const py_args& a) { return on_topic(i, a, "boss"); });
        put("on_snapshot", [this](interpreter&, const py_args& a) { return on_topic(i, a, "snapshot"); });
        put("on_encounter_finalized", [this](interpreter&, const py_args& a) { return on_topic(i, a, "encounter_finalized"); });
        put("emit", [this](interpreter&, const py_args& a) { return m_emit(i, a); });
        put("get_snapshot", [this](interpreter&, const py_args& a) { return m_get_snapshot(i, a); });
        put("snapshot_value", [this](interpreter&, const py_args& a) { return m_snapshot_value(i, a); });
        put("recent_events", [this](interpreter&, const py_args& a) { return m_recent_events(i, a); });

        // settings
        put("get_setting", [this](interpreter&, const py_args& a) { return m_get_setting(i, a); });
        put("setting", [this](interpreter&, const py_args& a) { return m_get_setting(i, a); });
        put("set_setting", [this](interpreter&, const py_args& a) { return m_set_setting(i, a); });
        put("set_defaults", [this](interpreter&, const py_args& a) { return m_set_defaults(i, a); });

        // registrations
        put("register_parser_adapter", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "parser_adapter"); });
        put("register_exporter", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "exporter"); });
        put("register_formatter", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "formatter"); });
        put("register_trigger_type", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "trigger_type"); });
        put("register_report_view", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "report_view"); });
        put("register_timer", [this](interpreter&, const py_args& a) { return m_register_extension(i, a, "timer_ext"); });
        put("register_ui_panel", [this](interpreter&, const py_args& a) { return m_register_ui_panel(i, a); });
        put("register_render_hook", [this](interpreter&, const py_args& a) { return m_render_hook(i, a); });
        put("unregister_render_hook", [this](interpreter&, const py_args& a) { return m_unregister_render_hook(i, a); });
        put("set_overlay", [this](interpreter&, const py_args& a) { return m_set_overlay(i, a); });
        put("clear_overlay", [this](interpreter&, const py_args& a) { return m_clear_overlay(i, a); });
        put("request_redraw", [this](interpreter&, const py_args& a) { return m_request_redraw(i, a); });
        put("register_hotkey", [this](interpreter&, const py_args& a) { return m_register_hotkey(i, a); });
        put("unregister_hotkey", [this](interpreter&, const py_args& a) { return m_unregister_hotkey(i, a); });
        put("register_menu_category", [this](interpreter&, const py_args& a) { return m_menu_category(i, a); });
        put("register_menu_surface", [this](interpreter&, const py_args& a) { return m_menu_surface(i, a); });
        put("register_action_handler", [this](interpreter&, const py_args& a) { return m_register_action_handler(i, a); });
        put("register_engine", [this](interpreter&, const py_args& a) { return m_register_engine(i, a); });
        put("register_data_source", [this](interpreter&, const py_args& a) { return m_register_data_source(i, a); });
        put("register_thread", [this](interpreter&, const py_args& a) { return m_register_thread(i, a); });

        // timers
        put("set_interval", [this](interpreter&, const py_args& a) { return m_set_interval(i, a); });
        put("set_timeout", [this](interpreter&, const py_args& a) { return m_set_timeout(i, a); });
        put("clear_timer", [this](interpreter&, const py_args& a) { return m_clear_timer(i, a); });
        put("complete_timer", [this](interpreter&, const py_args& a) { return m_complete_timer(i, a); });
        put("run_on_ui", [this](interpreter&, const py_args& a) { return m_run_on_ui(i, a); });

        // notify/dialog
        put("notify", [this](interpreter&, const py_args& a) { return m_notify(i, a); });
        put("dismiss_notify", [this](interpreter&, const py_args& a) { return m_dismiss_notify(i, a); });
        put("toast", [this](interpreter&, const py_args& a) { return m_toast(i, a); });
        put("open_file", [this](interpreter&, const py_args& a) { return m_open_file(i, a); });
        put("open_window", [this](interpreter&, const py_args& a) { return m_open_window(i, a); });

        // compositor
        put("create_compositor_layer", [this](interpreter&, const py_args& a) { return m_create_layer(i, a); });
        put("upload_compositor_frame", [this](interpreter&, const py_args& a) { return m_upload_frame(i, a); });
        put("set_compositor_layer_mmf_source", [this](interpreter&, const py_args& a) { return m_layer_mmf(i, a); });
        put("set_compositor_layer_shared_texture_source", [this](interpreter&, const py_args& a) { return m_layer_shared_tex(i, a); });
        put("set_compositor_layer_position", [this](interpreter&, const py_args& a) { return m_layer_pos(i, a); });
        put("set_compositor_layer_visible", [this](interpreter&, const py_args& a) { return m_layer_visible(i, a); });
        put("set_compositor_layer_input", [this](interpreter&, const py_args& a) { return m_layer_input(i, a); });
        put("destroy_compositor_layer", [this](interpreter&, const py_args& a) { return m_layer_destroy(i, a); });
        put("compositor_gpu_interop_available", [this](interpreter&, const py_args& a) { return m_gpu_interop(i, a); });
        put("compositor_layer_shared_texture_active", [this](interpreter&, const py_args& a) { return m_layer_shared_tex_active(i, a); });
        put("compositor_display_refresh_hz", [this](interpreter&, const py_args& a) { return m_layer_refresh(i, a); });

        // engine/deps
        put("get_engine", [this](interpreter&, const py_args& a) { return m_get_engine(i, a); });
        put("require_engine", [this](interpreter&, const py_args& a) { return m_require_engine(i, a); });
        put("call_engine", [this](interpreter&, const py_args& a) { return m_call_engine(i, a); });
        put("engine_call", [this](interpreter&, const py_args& a) { return m_engine_call(i, a); });
        put("call_runtime", [this](interpreter&, const py_args& a) { return m_call_runtime(i, a); });
        put("ensure_requirements", [this](interpreter&, const py_args& a) { return m_ensure_requirements(i, a); });
        put("load_local", [this](interpreter&, const py_args& a) { return m_load_local(i, a); });
        put("open", [this](interpreter&, const py_args& a) { return m_open(i, a); });
        put("time", [this](interpreter&, const py_args& a) { return m_time(i, a); });

        // sub-objects
        putv("ui", make_ui());
        putv("event_bus", make_event_bus());
        putv("engine", make_engine_obj());
        putv("mem", make_mem());
        putv("_ledgers", ledgers);

        return proxy;
    }
};

// surface notes
const char* const k_surface_names[] = {
    "plugin_id", "path", "web_path", "assets_path", "should_stop", "owner",
    "log",
    "subscribe", "subscribe_once", "unsubscribe", "on", "on_damage",
    "on_heal", "on_skill", "on_boss", "on_snapshot",
    "on_encounter_finalized", "emit", "get_snapshot", "snapshot_value",
    "recent_events",
    "get_setting", "setting", "set_setting", "set_defaults",
    "register_parser_adapter", "register_exporter", "register_formatter",
    "register_trigger_type", "register_report_view", "register_timer",
    "register_ui_panel", "register_render_hook", "unregister_render_hook",
    "set_overlay", "clear_overlay", "request_redraw",
    "register_hotkey", "unregister_hotkey", "register_menu_category",
    "register_menu_surface", "register_action_handler", "register_engine",
    "register_data_source", "register_thread",
    "set_interval", "set_timeout", "clear_timer", "complete_timer",
    "run_on_ui",
    "notify", "dismiss_notify", "toast", "open_file", "open_window",
    "create_compositor_layer", "upload_compositor_frame",
    "set_compositor_layer_position", "set_compositor_layer_visible",
    "set_compositor_layer_input", "destroy_compositor_layer",
    "set_compositor_layer_mmf_source", "set_compositor_layer_shared_texture_source",
    "compositor_gpu_interop_available",
    "compositor_layer_shared_texture_active",
    "compositor_display_refresh_hz",
    "get_engine", "require_engine", "call_engine", "call_runtime",
    "engine_call", "ensure_requirements", "load_local", "open", "time",
    "ui", "event_bus", "engine", "mem",
    // ui.* names recorded under their canonical spellings
    "ui.panel", "ui.section", "ui.card", "ui.row", "ui.group", "ui.text",
    "ui.title", "ui.kv", "ui.bar", "ui.slider", "ui.badge", "ui.divider",
    "ui.spacer", "ui.button", "ui.input", "ui.table", "ui.canvas",
    "ui.rgba_frame", "ui.rect", "ui.oval", "ui.line", "ui.ctext",
    // engine.* reflective surface entry points (catalog callables are
    // dynamic — each binds through engine_call/method_engine_call)
    "engine.list", "engine.on",
    nullptr,
};

} // namespace

// ── public entry: build ctx object bound to interpreter cfg ───────────
PyRef pymini_make_ctx(interpreter& i) {
    // heap-allocated + anchored: the method lambdas in build() capture the
    // raw `this`, so the builder must outlive the ctx dict.
    auto b = std::make_shared<ctx_builder>(
        ctx_builder{i, nullptr, py_dict(), py_dict(), {}});
    {
        std::lock_guard<std::mutex> g(g_cb_mu);
        g_ctx_builders[&i].push_back(b);
    }
    ctx_builder& br = *b;
    // ledgers scaffold (pyhost parity)
    auto* ld = as_dict(br.ledgers);
    dict_set(ld, py_str("panels"), py_dict());
    dict_set(ld, py_str("hotkeys"), py_dict());
    dict_set(ld, py_str("timers"), py_dict());
    dict_set(ld, py_str("render_hooks"), py_dict());
    dict_set(ld, py_str("extensions"), py_dict());
    // root path utf8
    {
        const std::wstring& w = i.cfg.plugin_root;
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                          static_cast<int>(w.size()), nullptr,
                                          0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()), s.data(), n, nullptr,
                            nullptr);
        br.root_utf8 = s;
    }
    script::ctx_surface_note_all(loader::engine_kind::python,
                                 k_surface_names);
    return br.build();
}

} // namespace sao::plugins::pymini
