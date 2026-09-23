// csmini_ctx.cpp — binds the canonical ~70-method `ctx` object into csmini
// modules.  Mirrors pymini_ctx.cpp 1:1 (same snake_case names, defaults,
// return shapes); each method calls the C ABI `sao_plugins_ctx_*` exports
// (or script_ctx shared helpers for ui.*/load_local).
//
// The C# subset has no kwargs — every kw-shaped parameter is positional in
// documented order:
//   register_ui_panel(id, meta, render, on_action)
//   register_data_source(id, meta, start, stop)
//   set_compositor_layer_input(name, cursor_pos, mouse_button,
//                              cursor_leave, scroll)
//   create_compositor_layer(name, w, h, x, y, z, click_through, high_fps,
//                           target_fps)
//   register_extension: kind is bound per method (parser_adapter/exporter/…)
// Callback-carrying registrations wrap CsRef callables in native trampolines
// held in g_cb_keep; teardown order (loader unregisters → interpreter dies)
// is safe via csmini_drop_callbacks.
//
// Reflective engine surface (sdk_binding/binding_engine.h contract):
// ctx.engine gains one named method per catalog entry ("mem.read_u64" →
// ctx.engine.mem_read_u64), plus engine.list(), engine.on(channel, cb) and
// the raw ctx.engine_call(name, dict) escape hatch.  Dispatch resolves via
// an owned SaoSdkContext — the loader-side plugin_context_t is SDK-agnostic
// by design, so csmini mirrors py_host's fallback / csharp_host's per-plugin
// path: sao_sdk_bind_context(plugin_id) + sao_sdk_context_bind_platform_
// services lazily on first use → sao_plugins_sdk_context_dispatch(…,
// sdk_method_id::method_engine_call, …).  The owned context is destroyed
// with the ctx builder at csmini_drop_callbacks.
#include "csmini_interp.h"

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_context_lifetime_internal.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_engine.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/menu_navigation.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"

#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_context.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_map>

namespace sao::plugins::csmini {

using sao::plugins::loader::plugin_context_t;
namespace script = sao::plugins::script_ctx;
namespace sdk_binding = sao::plugins::sdk_binding;

namespace {

// ═══ CsRef ↔ nlohmann::json ═══
CsRef cs_from_json(interpreter& i, const nlohmann::json& j) {
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return cs_null();
    case nlohmann::json::value_t::boolean:
        return cs_bool(j.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return cs_int(j.get<int64_t>());
    case nlohmann::json::value_t::number_float:
        return cs_float(j.get<double>());
    case nlohmann::json::value_t::string:
        return cs_str(j.get<std::string>());
    case nlohmann::json::value_t::array: {
        auto out = cs_array();
        for (const auto& x : j)
            as_array(out)->v.push_back(cs_from_json(i, x));
        return out;
    }
    case nlohmann::json::value_t::object: {
        auto d = cs_dict();
        for (auto it = j.begin(); it != j.end(); ++it)
            dict_set(d, cs_str(it.key()), cs_from_json(i, it.value()));
        return d;
    }
    default:
        return cs_null();
    }
}

nlohmann::json json_from_cs(interpreter& i, const CsRef& v, int depth = 0) {
    if (!v || depth > 24)
        return nullptr;
    switch (v->kind) {
    case cs_kind::null_:
        return nullptr;
    case cs_kind::boolean:
        return as_bool(v)->v;
    case cs_kind::integer:
        return as_int(v)->v;
    case cs_kind::char_:
        return as_char(v)->v;
    case cs_kind::number:
        return as_float(v)->v;
    case cs_kind::string:
        return as_str(v)->v;
    case cs_kind::guid:
        return cs_guid_format(as_guid(v)->bytes);
    case cs_kind::array: {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& x : as_array(v)->v)
            arr.push_back(json_from_cs(i, x, depth + 1));
        return arr;
    }
    case cs_kind::dict: {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, x] : as_dict(v)->items)
            obj[cs_to_str(i, k)] = json_from_cs(i, x, depth + 1);
        return obj;
    }
    case cs_kind::instance: {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, x] : as_dict(as_inst(v)->attrs)->items)
            if (auto* ks = as_str(k))
                obj[ks->v] = json_from_cs(i, x, depth + 1);
        return obj;
    }
    default:
        return cs_to_str(i, v);
    }
}

std::string jstr(interpreter& i, const CsRef& v, int depth = 0) {
    return json_from_cs(i, v, depth).dump();
}

CsRef parse_json(interpreter& i, const char* s) {
    if (!s || !*s)
        return cs_null();
    try {
        return cs_from_json(i, nlohmann::json::parse(s));
    } catch (...) {
        return cs_null();
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

// positional helpers (no kwargs in the C# subset)
CsRef pos_or(const cs_args& a, std::size_t n, CsRef dflt = nullptr) {
    return n < a.pos.size() ? a.pos[n] : dflt;
}
std::string pos_str(interpreter& i, const cs_args& a, std::size_t n,
                    const std::string& dflt = "") {
    if (n < a.pos.size())
        return cs_to_str(i, a.pos[n]);
    return dflt;
}
double pos_num(interpreter& i, const cs_args& a, std::size_t n,
               double dflt = 0.0) {
    if (n < a.pos.size()) {
        bool ok = false;
        const double d = cs_to_float(a.pos[n], &ok);
        if (ok)
            return d;
    }
    return dflt;
}
bool pos_bool(interpreter& i, const cs_args& a, std::size_t n, bool dflt) {
    if (n < a.pos.size())
        return cs_truthy(a.pos[n]);
    return dflt;
}
std::string narrow_w(const std::wstring& w) {
    if (w.empty())
        return {};
    std::string out;
    out.reserve(w.size());
    for (std::size_t k = 0; k < w.size(); ++k) {
        uint32_t cp = static_cast<uint16_t>(w[k]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // high surrogate — join with the low half into a supplementary
            // codepoint so we emit real 4-byte UTF-8 (not CESU-8 units).
            if (k + 1 < w.size()) {
                const uint32_t lo = static_cast<uint16_t>(w[k + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++k;
                } else {
                    cp = 0xFFFD;            // lone high surrogate
                }
            } else {
                cp = 0xFFFD;                // trailing high surrogate
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;                    // lone low surrogate
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// ═══ callback keep-list + trampolines ═══
struct cb_box {
    interpreter* i = nullptr;
    CsRef fn;
    std::string aux;                            // e.g. panel id
};
using cb_ptr = std::unique_ptr<cb_box>;
std::mutex g_cb_mu;
std::unordered_map<interpreter*, std::vector<cb_ptr>> g_cb_keep;

struct ctx_builder;
std::unordered_map<interpreter*,
                   std::vector<std::shared_ptr<ctx_builder>>>
    g_ctx_builders;

cb_box* keep_cb(interpreter& i, CsRef fn, std::string aux = {}) {
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

// csmini_host calls this at interpreter teardown (after the loader removed
// every registration that could still deliver callbacks).
void csmini_drop_callbacks(interpreter& i) {
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

// Trampoline entry points — each grabs the interpreter guard, marshals,
// calls, swallows errors.
void tr_event(const char* topic, const char* event_json, void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return;
    cs_guard g(*b->i);
    try {
        CsRef payload = parse_json(*b->i, event_json);
        cs_args a;
        a.pos.push_back(cs_str(topic ? topic : ""));
        a.pos.push_back(payload);
        (void)b->i->call(b->fn, a, {});
    } catch (...) {
    }
}
void tr_timer(void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return;
    cs_guard g(*b->i);
    try {
        b->i->call0(b->fn, {});
    } catch (...) {
    }
}
void tr_hotkey(void* ud) { tr_timer(ud); }
int32_t tr_render_hook(const char* surface, const char* payload_json,
                       char** out_spec_json, void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn || !out_spec_json)
        return -1;
    *out_spec_json = nullptr;
    cs_guard g(*b->i);
    try {
        CsRef payload = parse_json(*b->i, payload_json);
        cs_args a;
        a.pos.push_back(cs_str(surface ? surface : ""));
        a.pos.push_back(payload);
        CsRef r = b->i->call(b->fn, a, {});
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
    return tr_render_hook(action_id, payload_json, out_result_json, ud);
}
int32_t tr_data_source(void* ud) {
    auto* b = static_cast<cb_box*>(ud);
    if (!b || !b->i || !b->fn)
        return 0;
    cs_guard g(*b->i);
    try {
        b->i->call0(b->fn, {});
    } catch (...) {
        return -1;
    }
    return 0;
}

// token "topic:hex" (v1 shape)
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
        *out = static_cast<uint32_t>(
            std::stoul(tok.substr(pos + 1), nullptr, 16));
    } catch (...) {
        return false;
    }
    return true;
}

namespace menu_nav = script::menu_navigation;

bool menu_callable(const CsRef& value) {
    return value && (value->kind == cs_kind::func || value->kind == cs_kind::builtin ||
                     value->kind == cs_kind::bound_method);
}

std::string menu_callable_key(const CsRef& value) {
    if (value->kind == cs_kind::bound_method) {
        const auto* bound = static_cast<CsBoundMethodObj*>(value.get());
        return std::to_string(reinterpret_cast<uintptr_t>(bound->fn.get())) + ":" +
               std::to_string(reinterpret_cast<uintptr_t>(bound->self.get()));
    }
    return std::to_string(reinterpret_cast<uintptr_t>(value.get()));
}

struct menu_tree {
    std::vector<menu_nav::Node> roots;
    std::vector<std::vector<menu_nav::Node>> pending;
    menu_tree() { pending.reserve(menu_nav::max_nodes + 1); }
    ~menu_tree() {
        pending.push_back(std::move(roots));
        while (!pending.empty()) {
            auto level = std::move(pending.back());
            pending.pop_back();
            for (auto& node : level)
                if (!node.children.empty()) pending.push_back(std::move(node.children));
        }
    }
};

struct menu_values {
    std::vector<CsRef> values;
    ~menu_values() { for (auto& value : values) value.reset(); }
};

struct menu_state {
    interpreter* i{};
    CsRef builder;
    std::shared_ptr<CsRef> action_handler;
    std::string id, root, name, icon;
    double priority{};
    menu_nav::State navigation;
    struct identity {
        CsRef callable;
        std::string id;
        uint64_t seen{};
    };
    struct candidate {
        std::vector<menu_nav::Row> rows;
        uint64_t revision{};
        std::shared_ptr<menu_state> prepared;
        uint64_t base_generation{};
        bool committed{};
    };
    std::unordered_map<std::string, identity> identities;
    std::unordered_map<std::string, CsRef> commands;
    std::unordered_map<std::thread::id, candidate> candidates;
    std::vector<menu_nav::Row> published;
    std::string tree_content;
    uint64_t next_id{}, revision{};
    uint64_t generation{};
    bool building{};

    void rebuild(menu_nav::State requested, uint64_t& issued_id) {
        struct work {
            CsRef value;
            std::vector<menu_nav::Node>* level{};
            bool exit{};
            std::vector<CsRef> items;
            size_t index{};
            bool sequence{};
        };
        menu_tree tree;
        menu_values values;
        std::vector<work> stack;
        std::unordered_set<const CsObj*> ancestors;
        std::unordered_map<std::string, size_t> occurrences;
        std::unordered_map<const std::vector<menu_nav::Node>*, std::unordered_map<std::string, size_t>> labels;
        std::unordered_map<const std::vector<menu_nav::Node>*, size_t> parents{{&tree.roots, 0}};
        auto semantics = nlohmann::json::array();
        auto next_identities = identities;
        std::unordered_map<std::string, CsRef> next_commands;
        std::vector<std::string> seen;
        size_t nodes = 0, expansions = 0, bytes = 0;
        const auto fail = [&](const std::string& message) {
            i->raise_exc("ArgumentException", message, {});
        };
        const auto text = [&](const std::string& value, size_t maximum) {
            if (value.find('\0') != std::string::npos || value.size() > maximum ||
                value.size() > menu_nav::max_text_bytes - bytes)
                fail("menu text budget exceeded or embedded NUL");
            bytes += value.size();
        };
        text(id, 1024); text(name, 1024); text(icon, 256);
        stack.push_back({builder, &tree.roots});
        while (!stack.empty()) {
            work current = std::move(stack.back());
            stack.pop_back();
            if (current.exit) {
                ancestors.erase(current.value.get());
                if (current.level) { labels.erase(current.level); parents.erase(current.level); }
                continue;
            }
            if (current.sequence) {
                if (current.index < current.items.size()) {
                    const auto value = current.items[current.index++];
                    auto* level = current.level;
                    stack.push_back(std::move(current));
                    stack.push_back({value, level});
                }
                continue;
            }
            if (cs_is_null(current.value) || !ancestors.insert(current.value.get()).second)
                fail("invalid menu value or ancestor cycle");
            values.values.push_back(current.value);
            stack.push_back({current.value, nullptr, true});
            if (menu_callable(current.value)) {
                if (++expansions > 2 * (menu_nav::max_nodes + 1)) fail("menu expansion budget exceeded");
                stack.push_back({i->call0(current.value, {}), current.level});
                continue;
            }
            if (auto* array = as_array(current.value)) {
                if (++expansions > 2 * (menu_nav::max_nodes + 1) || array->v.size() > menu_nav::max_nodes)
                    fail("menu sequence budget exceeded");
                current.items = array->v;
                current.sequence = true;
                stack.push_back(std::move(current));
                continue;
            }
            const auto* item = as_dict(current.value);
            if (!item || ++nodes > menu_nav::max_nodes) fail("invalid menu row or node budget exceeded");
            const auto get = [&](const char* key) { return dict_get(item, cs_str(key)); };
            const auto string = [&](const char* key) {
                const auto value = get(key);
                return cs_is_null(value) ? std::string{} : cs_to_str(*i, value);
            };
            menu_nav::Node node;
            node.row.label = string("label");
            node.row.icon = string("icon");
            CsRef children;
            for (const auto* key : {"children", "items", "submenu"}) {
                children = get(key);
                if (children) { node.submenu = true; break; }
            }
            const CsRef command = get("command");
            const bool callable = menu_callable(command);
            if (!cs_is_null(command) && !callable) fail("menu command is not callable");
            const auto delegate = node.submenu && menu_callable(children) ? children : command;
            const std::string base = menu_callable(delegate) ? "fn:" + menu_callable_key(delegate) :
                "label:" + node.row.label;
            const std::string identity_key = base + ":" + std::to_string(occurrences[base]++);
            auto retained = next_identities.find(identity_key);
            if (retained == next_identities.end()) {
                if (issued_id == UINT64_MAX) fail("menu identity exhausted");
                retained = next_identities.emplace(identity_key,
                    identity{delegate, "menu-" + std::to_string(++issued_id), 0}).first;
            }
            seen.push_back(identity_key);
            const std::string explicit_id = string("id");
            if (!explicit_id.empty()) node.key = "id:" + explicit_id;
            else if (menu_callable(delegate)) node.key = retained->second.id;
            else node.key = "label:" + std::to_string(labels[current.level][node.row.label]++) + ":" + node.row.label;
            node.row.action_id = string("action_id");
            if (node.row.action_id.empty()) node.row.action_id = string("action");
            const bool explicit_action = !node.row.action_id.empty();
            if (node.row.action_id.starts_with(menu_nav::navigation_prefix))
                fail("reserved menu action namespace");
            if (node.row.action_id.empty()) node.row.action_id = retained->second.id;
            const auto enabled = get("can_activate");
            node.row.can_activate = (node.submenu || callable || explicit_action) &&
                (cs_is_null(enabled) || cs_truthy(enabled));
            node.row.keep_open = cs_truthy(get("keep_menu_open"));
            node.row.close_before = cs_truthy(get("close_menu_before"));
            const auto payload = get("payload");
            node.row.payload_json = cs_is_null(payload) ? "{}" : jstr(*i, payload);
            text(node.key, 1024); text(node.row.label, node.submenu ? 1019 : 1024); text(node.row.icon, 256);
            text(node.row.action_id, 1024); text(node.row.payload_json, 16384);
            semantics.push_back({parents.at(current.level), node.key, node.submenu, node.row.label,
                node.row.icon, node.row.action_id, node.row.payload_json, node.row.can_activate,
                node.row.keep_open, node.row.close_before});
            if (!node.submenu && callable) next_commands.emplace(node.row.action_id, command);
            current.level->push_back(std::move(node));
            if (current.level->back().submenu) {
                parents[&current.level->back().children] = nodes;
                stack.back().level = &current.level->back().children;
                stack.push_back({children, &current.level->back().children});
            }
        }
        std::string error;
        if (!requested.replace(tree.roots, error)) fail(error);
        auto rows = requested.rows();
        auto content = semantics.dump();
        uint64_t next_revision = revision;
        if (revision == 0 || rows != published || content != tree_content) {
            if (revision == UINT64_MAX) fail("menu revision exhausted");
            ++next_revision;
        }
        for (const auto& key : seen) next_identities.at(key).seen = next_revision;
        for (auto it = next_identities.begin(); it != next_identities.end();) {
            if (next_revision - it->second.seen >= 64) it = next_identities.erase(it);
            else ++it;
        }
        navigation = std::move(requested);
        published = std::move(rows);
        tree_content = std::move(content);
        identities.swap(next_identities);
        commands.swap(next_commands);
        revision = next_revision;
    }

    std::shared_ptr<menu_state> prepare(menu_nav::State requested) {
        if (building || generation == UINT64_MAX)
            i->raise_exc("InvalidOperationException", "menu rebuild is reentrant or generation exhausted", {});
        struct rebuild_guard {
            bool& active;
            explicit rebuild_guard(bool& value) : active(value) { active = true; }
            ~rebuild_guard() { active = false; }
        } guard(building);
        auto next = std::make_shared<menu_state>();
        next->i = i;
        next->builder = builder;
        next->id = id;
        next->name = name;
        next->icon = icon;
        next->priority = priority;
        next->identities = identities;
        next->published = published;
        next->tree_content = tree_content;
        next->revision = revision;
        next->rebuild(std::move(requested), next_id);
        return next;
    }

    void commit(candidate& snapshot) {
        if (snapshot.committed) return;
        auto& next = *snapshot.prepared;
        navigation = std::move(next.navigation);
        published.swap(next.published);
        tree_content.swap(next.tree_content);
        identities.swap(next.identities);
        commands.swap(next.commands);
        revision = next.revision;
        ++generation;
        snapshot.committed = true;
        snapshot.prepared.reset();
    }
};

int32_t SAO_PLUGINS_CALL menu_snapshot(void* rows, uint32_t capacity, uint32_t stride,
    uint32_t* count, uint64_t* revision, loader::entity_snapshot_content_token_t* token,
    uint32_t* out_stride, void* user_data) {
    if (!user_data || !count || !revision || !token || !out_stride) return SAO_ERR_INVALID_ARGUMENT;
    auto& menu = *static_cast<menu_state*>(user_data);
    try {
        cs_guard guard(*menu.i);
        if (menu.building) return SAO_ERR_INVALID_ARGUMENT;
        const auto thread = std::this_thread::get_id();
        const bool probe = !rows && capacity == 0 && stride == 0;
        if (probe) {
            auto next = menu.prepare(menu.navigation);
            menu_state::candidate candidate{next->published, next->revision, next, menu.generation};
            menu.candidates.insert_or_assign(thread, std::move(candidate));
        }
        const auto found = menu.candidates.find(thread);
        if (found == menu.candidates.end()) return SAO_ERR_INVALID_ARGUMENT;
        auto& snapshot = found->second;
        *count = static_cast<uint32_t>(snapshot.rows.size());
        *revision = snapshot.revision;
        *token = snapshot.revision;
        *out_stride = snapshot.rows.empty() ? 0 : probe ? sizeof(loader::entity_menu_row_v2) : stride;
        if (!snapshot.committed && snapshot.base_generation != menu.generation)
            return SAO_ERR_BUFFER_TOO_SMALL;
        if (snapshot.rows.empty()) { menu.commit(snapshot); return SAO_OK; }
        if (probe || !rows || capacity < *count || stride < sizeof(loader::entity_menu_row_v2) ||
            stride % alignof(loader::entity_menu_row_v2) != 0 ||
            uint64_t(stride) * *count > (std::numeric_limits<int32_t>::max)())
            return SAO_ERR_BUFFER_TOO_SMALL;
        for (size_t index = 0; index < snapshot.rows.size(); ++index) {
            const auto& source = snapshot.rows[index];
            const loader::entity_menu_row_v2 row{
                sizeof(loader::entity_menu_row_v2), menu.id.c_str(), menu.name.c_str(), menu.icon.c_str(),
                menu.priority, source.label.c_str(), source.icon.c_str(), source.action_id.c_str(),
                source.payload_json.c_str(), static_cast<uint8_t>(source.can_activate),
                static_cast<uint8_t>(source.keep_open), static_cast<uint8_t>(source.close_before), {}};
            std::memcpy(static_cast<uint8_t*>(rows) + index * stride, &row, sizeof(row));
        }
        menu.commit(snapshot);
        return SAO_OK;
    } catch (...) { return SAO_ERR_OS_CALL_FAILED; }
}

int32_t SAO_PLUGINS_CALL menu_action(const char* action, const char* payload,
    loader::entity_action_result_sink_v2_fn sink, void* sink_data, void* user_data) {
    if (!action || !payload || !sink || !user_data) return SAO_ERR_INVALID_ARGUMENT;
    auto& menu = *static_cast<menu_state*>(user_data);
    try {
        cs_guard guard(*menu.i);
        if (menu.building) return SAO_ERR_INVALID_ARGUMENT;
        bool handled = false;
        std::string output;
        auto navigation = menu.navigation;
        const auto route = navigation.activate(action);
        if (route == menu_nav::NavigationResult::handled) {
            auto next = menu.prepare(std::move(navigation));
            menu_state::candidate candidate{{}, next->revision, next, menu.generation};
            menu.commit(candidate);
            handled = true;
        } else if (route == menu_nav::NavigationResult::not_navigation) {
            const auto row = std::find_if(menu.published.begin(), menu.published.end(), [&](const auto& value) {
                return value.action_id == action && value.can_activate;
            });
            if (row != menu.published.end()) {
                const auto command = menu.commands.find(action);
                CsRef result;
                if (command != menu.commands.end()) {
                    const auto fn = command->second;
                    result = menu.i->call0(fn, {});
                    handled = true;
                } else if (menu.action_handler && menu_callable(*menu.action_handler)) {
                    const auto fn = *menu.action_handler;
                    cs_args args;
                    args.pos = {cs_str(action), parse_json(*menu.i, payload)};
                    result = menu.i->call(fn, args, {});
                    handled = true;
                }
                if (handled) output = jstr(*menu.i, result);
            }
        }
        const loader::entity_action_result_v2 result{sizeof(loader::entity_action_result_v2),
            loader::kEntityActionAbiVersion2, static_cast<uint8_t>(handled), {},
            handled && !output.empty() ? output.c_str() : nullptr};
        return sink(&result, sink_data);
    } catch (...) { return SAO_ERR_OS_CALL_FAILED; }
}

// ═══ ctx builder ═══
struct ctx_builder {
    interpreter& i;
    CsRef ui_obj;
    CsRef engines;          // dict name→engine object (get_engine lookup)
    CsRef ledgers;          // {"panels": {}, "hotkeys": {}, "timers": {}, ...}
    std::string root_utf8;
    std::vector<std::unique_ptr<menu_state>> menus;
    std::shared_ptr<CsRef> menu_action_handler = std::make_shared<CsRef>();

    explicit ctx_builder(interpreter& value) : i(value), engines(cs_dict()), ledgers(cs_dict()) {}

    // ── reflective engine surface state (binding_engine.h contract) ──
    // Lazily bound, builder-owned SaoSdkContext used to reach
    // sao_plugins_sdk_context_dispatch(…, method_engine_call, …).  Destroyed
    // with the builder when g_ctx_builders drops at csmini_drop_callbacks.
    SaoSdkContext engine_sdk_ctx{};
    bool engine_ctx_ready = false;
    bool engine_ctx_failed = false;
    // engine.on(channel, cb) registrations: channel name → keep_cb box.
    std::unordered_map<std::string, cb_box*> engine_channels;

    ~ctx_builder() {
        if (engine_sdk_ctx.ctx_impl != nullptr)
            sao_sdk_context_destroy(&engine_sdk_ctx);
    }

    plugin_context_t* c() { return need_ctx(i); }

    CsRef method(const char* n, cs_native_fn f) {
        return cs_builtin(n, std::move(f));
    }

    // ── simple JSON pass-throughs ────────────────────────────────────
    CsRef m_log(interpreter&, const cs_args& a) {
        const std::string msg = a.pos.empty() ? "" : cs_to_str(i, a.pos[0]);
        sao_plugins_ctx_log(c(), msg.c_str());
        return cs_null();
    }
    CsRef m_emit(interpreter&, const cs_args& a) {
        const std::string topic = pos_str(i, a, 0);
        const std::string payload =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("{}");
        sao_plugins_ctx_emit(c(), topic.c_str(), payload.c_str());
        return cs_null();
    }
    CsRef m_get_snapshot(interpreter&, const cs_args&) {
        char* raw = nullptr;
        if (sao_plugins_ctx_get_snapshot(c(), &raw) != 0 || !raw)
            return cs_dict();
        CsRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    CsRef m_snapshot_value(interpreter&, const cs_args& a) {
        const std::string path = pos_str(i, a, 0);
        char* raw = nullptr;
        if (sao_plugins_ctx_snapshot_value(c(), path.c_str(), &raw) != 0 ||
            !raw) {
            return pos_or(a, 1, cs_null());
        }
        CsRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    CsRef m_recent_events(interpreter&, const cs_args& a) {
        bool ok = false;
        const int64_t limit =
            a.pos.empty() ? 20 : cs_to_int(a.pos[0], &ok);
        const std::string topic = pos_str(i, a, 1);
        char* raw = nullptr;
        if (sao_plugins_ctx_recent_events(c(), static_cast<uint32_t>(limit),
                                          topic.c_str(), &raw) != 0 ||
            !raw)
            return cs_array();
        CsRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    CsRef m_get_setting(interpreter&, const cs_args& a) {
        const std::string key = pos_str(i, a, 0);
        char* raw = nullptr;
        if (sao_plugins_ctx_get_setting(c(), key.c_str(), &raw) != 0 || !raw)
            return pos_or(a, 1, cs_null());
        CsRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    CsRef m_set_setting(interpreter&, const cs_args& a) {
        const std::string key = pos_str(i, a, 0);
        const std::string val =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("null");
        sao_plugins_ctx_set_setting(c(), key.c_str(), val.c_str());
        return cs_null();
    }
    CsRef m_set_defaults(interpreter&, const cs_args& a) {
        if (!a.pos.empty())
            sao_plugins_ctx_set_defaults(c(), jstr(i, a.pos[0]).c_str());
        return cs_null();
    }

    // ── events with callbacks ────────────────────────────────────────
    CsRef m_subscribe(interpreter&, const cs_args& a) {
        const std::string topic = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (!cb || (cb->kind != cs_kind::func && cb->kind != cs_kind::builtin &&
                    cb->kind != cs_kind::bound_method))
            i.raise_exc("ArgumentException",
                        "subscribe() missing callback", {});
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_subscribe(
            c(), topic.c_str(), tr_event, b, &token);
        if (r != 0)
            return cs_null();
        return cs_str(fmt_token(topic, token));
    }
    CsRef m_subscribe_once(interpreter&, const cs_args& a) {
        const std::string topic = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (!cb || (cb->kind != cs_kind::func && cb->kind != cs_kind::builtin &&
                    cb->kind != cs_kind::bound_method))
            i.raise_exc("ArgumentException",
                        "subscribe_once() missing callback", {});
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_subscribe_once(
            c(), topic.c_str(), tr_event, b, &token);
        if (r != 0)
            return cs_null();
        return cs_str(fmt_token(topic, token));
    }
    CsRef m_unsubscribe(interpreter&, const cs_args& a) {
        const std::string tok = pos_str(i, a, 0);
        uint32_t t = 0;
        if (parse_token(tok, &t))
            sao_plugins_ctx_unsubscribe(c(), t);
        return cs_null();
    }
    // ctx.on(topic) — direct form on(topic, cb) or decorator-style returns
    // are unsupported (no delegates) — but subscribing a bound method or
    // a user function works the same.
    CsRef m_on(interpreter&, const cs_args& a) {
        const std::string topic = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (cb) {
            cb_box* b = keep_cb(i, cb);
            uint32_t token = 0;
            sao_plugins_ctx_subscribe(c(), topic.c_str(), tr_event, b,
                                      &token);
            return cb;
        }
        // decorator-ish form: return a builtin that subscribes its arg
        return cs_builtin("on_decorator",
                          [this, topic](interpreter&, const cs_args& a2) {
                              CsRef cb2 =
                                  a2.pos.empty() ? nullptr : a2.pos[0];
                              if (cb2) {
                                  cb_box* b = keep_cb(i, cb2);
                                  uint32_t token = 0;
                                  sao_plugins_ctx_subscribe(
                                      c(), topic.c_str(), tr_event, b,
                                      &token);
                              }
                              return cb2 ? cb2 : cs_null();
                          });
    }
    CsRef on_topic(interpreter&, const cs_args& a, const char* topic) {
        CsRef cb = pos_or(a, 0);
        if (!cb) {
            return cs_builtin(
                "on_decorator",
                [this, t = std::string(topic)](interpreter&,
                                               const cs_args& a2) {
                    CsRef c2 = a2.pos.empty() ? nullptr : a2.pos[0];
                    if (c2) {
                        cb_box* b = keep_cb(i, c2);
                        uint32_t token = 0;
                        sao_plugins_ctx_subscribe(c(), t.c_str(), tr_event,
                                                  b, &token);
                    }
                    return c2 ? c2 : cs_null();
                });
        }
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        sao_plugins_ctx_subscribe(c(), topic, tr_event, b, &token);
        return cb;
    }

    // ── timers ───────────────────────────────────────────────────────
    CsRef m_set_interval(interpreter&, const cs_args& a) {
        CsRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("ArgumentException",
                        "set_interval() missing callback", {});
        const double seconds =
            a.pos.size() > 1 ? pos_num(i, a, 1) : 0.0;
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        const int32_t r = sao_plugins_ctx_set_interval(
            c(), tr_timer, seconds, b, &token_raw);
        if (r != 0 || !token_raw)
            return cs_null();
        CsRef out = cs_str(token_raw);
        free_ctx_str(token_raw);
        return out;
    }
    CsRef m_set_timeout(interpreter&, const cs_args& a) {
        CsRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("ArgumentException",
                        "set_timeout() missing callback", {});
        const double seconds =
            a.pos.size() > 1 ? pos_num(i, a, 1) : 0.0;
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        const int32_t r = sao_plugins_ctx_set_timeout(c(), tr_timer, seconds,
                                                      b, &token_raw);
        if (r != 0 || !token_raw)
            return cs_null();
        CsRef out = cs_str(token_raw);
        free_ctx_str(token_raw);
        return out;
    }
    CsRef m_clear_timer(interpreter&, const cs_args& a) {
        const std::string tok = pos_str(i, a, 0);
        if (!tok.empty())
            sao_plugins_ctx_clear_timer(c(), tok.c_str());
        return cs_null();
    }
    CsRef m_complete_timer(interpreter&, const cs_args& a) {
        const std::string tok = pos_str(i, a, 0);
        if (!tok.empty())
            sao_plugins_ctx_complete_timer(c(), tok.c_str());
        return cs_null();
    }
    CsRef m_run_on_ui(interpreter&, const cs_args& a) {
        CsRef cb = pos_or(a, 0);
        if (!cb)
            return cs_null();
        cb_box* b = keep_cb(i, cb);
        char* token_raw = nullptr;
        sao_plugins_ctx_set_timeout(c(), tr_timer, 0.0, b, &token_raw);
        if (token_raw)
            free_ctx_str(token_raw);
        return cs_null();
    }

    // ── notify / dialogs / windows ───────────────────────────────────
    CsRef m_notify(interpreter&, const cs_args& a) {
        const std::string title = pos_str(i, a, 0);
        const std::string msg = pos_str(i, a, 1);
        const double dur = pos_num(i, a, 2, 60.0);
        const std::string kind = pos_str(i, a, 3, "plugin");
        sao_plugins_ctx_notify(c(), title.c_str(), msg.c_str(), dur,
                               kind.c_str());
        return cs_null();
    }
    CsRef m_dismiss_notify(interpreter&, const cs_args&) {
        sao_plugins_ctx_dismiss_notify(c());
        return cs_null();
    }
    CsRef m_toast(interpreter&, const cs_args& a) {
        const std::string msg = pos_str(i, a, 0);
        sao_plugins_ctx_toast(c(), msg.c_str());
        return cs_null();
    }
    CsRef m_open_file(interpreter&, const cs_args& a) {
        std::string filters = "[]";
        std::string title;
        if (!a.pos.empty()) {
            if (auto* s = as_str(a.pos[0]))
                filters = s->v;
            else
                filters = jstr(i, a.pos[0]);
        }
        title = pos_str(i, a, 1);
        wchar_t* path = nullptr;
        const int32_t r = sao_plugins_ctx_open_file(
            c(), filters.c_str(), title.c_str(), nullptr, 0, &path);
        if (r != 0 || !path)
            return cs_null();
        CsRef out = cs_str(narrow_w(path));
        loader::sao_plugins_ctx_free_wstring(path);
        return out;
    }
    CsRef m_open_window(interpreter&, const cs_args& a) {
        const std::string pid = pos_str(i, a, 0);
        const uint32_t w = static_cast<uint32_t>(pos_num(i, a, 1));
        const uint32_t h = static_cast<uint32_t>(pos_num(i, a, 2));
        const int32_t r = sao_plugins_ctx_open_window(
            c(), pid.empty() ? nullptr : pid.c_str(), w, h);
        return cs_bool(r == 0);
    }

    // ── compositor family ────────────────────────────────────────────
    CsRef m_create_layer(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        const uint32_t w = static_cast<uint32_t>(pos_num(i, a, 1));
        const uint32_t h = static_cast<uint32_t>(pos_num(i, a, 2));
        const int32_t x = static_cast<int32_t>(pos_num(i, a, 3));
        const int32_t y = static_cast<int32_t>(pos_num(i, a, 4));
        const int32_t z = static_cast<int32_t>(pos_num(i, a, 5, 140));
        const bool click_through = pos_bool(i, a, 6, true);
        const bool high_fps = pos_bool(i, a, 7, false);
        const uint32_t target_fps =
            static_cast<uint32_t>(pos_num(i, a, 8, 0));
        const int32_t r = sao_plugins_ctx_create_compositor_layer(
            c(), name.c_str(), w, h, x, y, z, click_through, high_fps,
            target_fps);
        return cs_bool(r == 0);
    }
    CsRef m_upload_frame(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        std::string bytes;
        if (a.pos.size() > 1) {
            if (auto* s = as_str(a.pos[1])) {
                bytes = s->v;
            } else if (auto* l = as_array(a.pos[1])) {
                bytes.reserve(l->v.size());
                for (const auto& x : l->v) {
                    bool ok = false;
                    bytes += static_cast<char>(cs_to_int(x, &ok));
                }
            }
        }
        const uint32_t w = static_cast<uint32_t>(pos_num(i, a, 2));
        const uint32_t h = static_cast<uint32_t>(pos_num(i, a, 3));
        const int32_t r = sao_plugins_ctx_upload_compositor_frame(
            c(), name.c_str(),
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), w,
            h);
        if (r == 0 && a.pos.size() > 5) {
            const int32_t x = static_cast<int32_t>(pos_num(i, a, 4));
            const int32_t y = static_cast<int32_t>(pos_num(i, a, 5));
            sao_plugins_ctx_set_compositor_layer_position(c(), name.c_str(),
                                                          x, y);
        }
        return cs_bool(r == 0);
    }
    CsRef m_layer_pos(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        const int32_t x = static_cast<int32_t>(pos_num(i, a, 1));
        const int32_t y = static_cast<int32_t>(pos_num(i, a, 2));
        return cs_bool(sao_plugins_ctx_set_compositor_layer_position(
                           c(), name.c_str(), x, y) == 0);
    }
    CsRef m_layer_visible(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        const bool v = a.pos.size() > 1 && cs_truthy(a.pos[1]);
        return cs_bool(sao_plugins_ctx_set_compositor_layer_visible(
                           c(), name.c_str(), v) == 0);
    }
    CsRef m_layer_destroy(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        return cs_bool(sao_plugins_ctx_destroy_compositor_layer(
                           c(), name.c_str()) == 0);
    }
    // positional form: (name, cursor_pos, mouse_button, cursor_leave, scroll)
    CsRef m_layer_input(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        loader::compositor_cursor_pos_fn cp = nullptr;
        loader::compositor_mouse_button_fn mb = nullptr;
        loader::compositor_cursor_leave_fn cl = nullptr;
        loader::compositor_scroll_fn sc = nullptr;
        cb_box* carrier = keep_cb(i, cs_dict());
        auto* cd = as_dict(carrier->fn);
        auto set_cb = [&](const char* k, CsRef v) {
            if (v && v->kind != cs_kind::null_)
                dict_set(cd, cs_str(k), v);
        };
        set_cb("cursor_pos", pos_or(a, 1));
        set_cb("mouse_button", pos_or(a, 2));
        set_cb("cursor_leave", pos_or(a, 3));
        set_cb("scroll", pos_or(a, 4));
        if (dict_get(cd, cs_str("cursor_pos"))) {
            cp = [](float x, float y, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                cs_guard g(*b->i);
                auto* d = as_dict(b->fn);
                try {
                    cs_args a2;
                    a2.pos.push_back(cs_float(x));
                    a2.pos.push_back(cs_float(y));
                    (void)b->i->call(dict_get(d, cs_str("cursor_pos")),
                                     a2, {});
                } catch (...) {
                }
            };
        }
        if (dict_get(cd, cs_str("mouse_button"))) {
            mb = [](uint32_t btn, bool pressed, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                cs_guard g(*b->i);
                auto* d = as_dict(b->fn);
                try {
                    cs_args a2;
                    a2.pos.push_back(cs_int(btn));
                    a2.pos.push_back(cs_bool(pressed));
                    (void)b->i->call(dict_get(d, cs_str("mouse_button")),
                                     a2, {});
                } catch (...) {
                }
            };
        }
        if (dict_get(cd, cs_str("cursor_leave"))) {
            cl = [](void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                cs_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn),
                                         cs_str("cursor_leave")),
                                {});
                } catch (...) {
                }
            };
        }
        if (dict_get(cd, cs_str("scroll"))) {
            sc = [](float dx, float dy, void* ud) {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return;
                cs_guard g(*b->i);
                try {
                    cs_args a2;
                    a2.pos.push_back(cs_float(dx));
                    a2.pos.push_back(cs_float(dy));
                    (void)b->i->call(dict_get(as_dict(b->fn),
                                              cs_str("scroll")),
                                     a2, {});
                } catch (...) {
                }
            };
        }
        const int32_t r = sao_plugins_ctx_set_compositor_layer_input(
            c(), name.c_str(), cp, mb, cl, sc, carrier);
        return cs_bool(r == 0);
    }
    CsRef compositor_result(const char* operation, int32_t status, bool result = true) {
        if (status != 0)
            i.raise_exc("InvalidOperationException", std::string(operation) +
                " failed with status " + std::to_string(status), {});
        return cs_bool(result);
    }
    std::string compositor_name(const cs_args& a, size_t index, bool allow_empty = false) {
        const auto* value = as_str(pos_or(a, index));
        if (!value || (!allow_empty && value->v.empty()) || value->v.find('\0') != std::string::npos)
            i.raise_exc("ArgumentException", "invalid compositor source or layer name", {});
        return value->v;
    }
    CsRef m_gpu_interop(interpreter&, const cs_args&) {
        bool available = false;
        const int32_t status = sao_plugins_ctx_compositor_gpu_interop_available(c(), &available);
        return compositor_result("compositor_gpu_interop_available", status, available);
    }
    CsRef m_layer_shared_tex(interpreter&, const cs_args& a) {
        const std::string name = compositor_name(a, 0);
        const auto integer = [&](size_t index, uint64_t maximum) -> uint64_t {
            const auto* value = as_int(pos_or(a, index));
            if (!value || value->v < 0 || static_cast<uint64_t>(value->v) > maximum)
                i.raise_exc("ArgumentException", "shared texture expects nonnegative integral handle and uint32 dimensions", {});
            return static_cast<uint64_t>(value->v);
        };
        const uint64_t handle = integer(1, UINT64_MAX);
        const uint32_t width = static_cast<uint32_t>(integer(2, UINT32_MAX));
        const uint32_t height = static_cast<uint32_t>(integer(3, UINT32_MAX));
        return compositor_result("set_compositor_layer_shared_texture_source",
            sao_plugins_ctx_set_compositor_layer_shared_texture_source(c(), name.c_str(), handle, width, height));
    }
    CsRef m_layer_mmf(interpreter&, const cs_args& a) {
        const std::string name = compositor_name(a, 0);
        const std::string mmf = compositor_name(a, 1, true);
        return compositor_result("set_compositor_layer_mmf_source",
            sao_plugins_ctx_set_compositor_layer_mmf_source(c(), name.c_str(), mmf.c_str()));
    }
    CsRef m_layer_refresh(interpreter&, const cs_args&) {
        return cs_float(0.0);
    }
    CsRef m_layer_shared_tex_active(interpreter&, const cs_args& a) {
        const std::string name = compositor_name(a, 0);
        bool active = false;
        const int32_t status = sao_plugins_ctx_compositor_layer_shared_texture_active(c(), name.c_str(), &active);
        return compositor_result("compositor_layer_shared_texture_active", status, active);
    }

    // ── registrations ────────────────────────────────────────────────
    CsRef m_register_hotkey(interpreter&, const cs_args& a) {
        const std::string id = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("ArgumentException",
                        "register_hotkey() missing callback", {});
        const std::string defkey = pos_str(i, a, 2);
        const std::string label = pos_str(i, a, 3);
        cb_box* b = keep_cb(i, cb);
        const int32_t r = sao_plugins_ctx_register_hotkey(
            c(), id.c_str(), defkey.c_str(), label.c_str(), tr_hotkey, b);
        return cs_bool(r == 0);
    }
    CsRef m_unregister_hotkey(interpreter&, const cs_args& a) {
        sao_plugins_ctx_unregister_hotkey(c(), pos_str(i, a, 0).c_str());
        return cs_null();
    }
    CsRef m_render_hook(interpreter&, const cs_args& a) {
        const std::string surface = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (!cb)
            i.raise_exc("ArgumentException",
                        "register_render_hook() missing callback", {});
        const float prio = static_cast<float>(pos_num(i, a, 2, 0.0));
        cb_box* b = keep_cb(i, cb);
        uint32_t token = 0;
        const int32_t r = sao_plugins_ctx_register_render_hook(
            c(), surface.c_str(), prio, tr_render_hook, b, &token);
        if (r != 0)
            return cs_null();
        return cs_str(fmt_token("hook", token));
    }
    CsRef m_unregister_render_hook(interpreter&, const cs_args& a) {
        uint32_t t = 0;
        if (parse_token(pos_str(i, a, 0), &t))
            sao_plugins_ctx_unregister_render_hook(c(), t);
        return cs_null();
    }
    CsRef m_set_overlay(interpreter&, const cs_args& a) {
        const std::string surface = pos_str(i, a, 0);
        const std::string spec =
            a.pos.size() > 1 ? jstr(i, a.pos[1]) : std::string("{}");
        return cs_bool(sao_plugins_ctx_set_overlay(c(), surface.c_str(),
                                                   spec.c_str()) == 0);
    }
    CsRef m_clear_overlay(interpreter&, const cs_args& a) {
        const std::string surface = pos_str(i, a, 0);
        return cs_bool(sao_plugins_ctx_clear_overlay(
                           c(), surface.empty() ? nullptr
                                                  : surface.c_str()) == 0);
    }
    CsRef m_request_redraw(interpreter&, const cs_args& a) {
        const std::string surface = pos_str(i, a, 0);
        const std::string reason = pos_str(i, a, 1);
        sao_plugins_ctx_request_redraw(c(), surface.c_str(),
                                       reason.empty() ? nullptr
                                                      : reason.c_str());
        return cs_null();
    }

    // positional form: register_ui_panel(id, meta, render, on_action)
    CsRef m_register_ui_panel(interpreter&, const cs_args& a) {
        const std::string id = pos_str(i, a, 0);
        CsRef meta = pos_or(a, 1, cs_dict());
        CsRef render = pos_or(a, 2);
        CsRef on_action = pos_or(a, 3);
        if (render && render->kind == cs_kind::null_)
            render = nullptr;
        if (on_action && on_action->kind == cs_kind::null_)
            on_action = nullptr;
        cb_box* carrier = keep_cb(i, cs_dict(), id);
        auto* cd = as_dict(carrier->fn);
        if (render)
            dict_set(cd, cs_str("render"), render);
        if (on_action)
            dict_set(cd, cs_str("on_action"), on_action);
        const int32_t r = sao_plugins_ctx_register_ui_panel(
            c(), id.c_str(), jstr(i, meta).c_str(),
            render ? [](const char* payload, char** out,
                        void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i || !out)
                    return -1;
                *out = nullptr;
                cs_guard g(*b->i);
                try {
                    CsRef fn =
                        dict_get(as_dict(b->fn), cs_str("render"));
                    cs_args a2;
                    a2.pos.push_back(parse_json(*b->i, payload));
                    CsRef r2 = b->i->call(fn, a2, {});
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
                cs_guard g(*b->i);
                try {
                    CsRef fn =
                        dict_get(as_dict(b->fn), cs_str("on_action"));
                    cs_args a2;
                    a2.pos.push_back(cs_str(action_id ? action_id : ""));
                    a2.pos.push_back(parse_json(*b->i, payload));
                    CsRef r2 = b->i->call(fn, a2, {});
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
                as_dict(dict_get(as_dict(ledgers), cs_str("panels")));
            if (panels) {
                auto rec = cs_dict();
                dict_set(rec, cs_str("id"), cs_str(id));
                dict_set(rec, cs_str("meta"), meta);
                dict_set(rec, cs_str("render"),
                         render ? render : cs_null());
                dict_set(rec, cs_str("on_action"),
                         on_action ? on_action : cs_null());
                dict_set(panels, cs_str(id), rec);
            }
        }
        return cs_bool(r == 0);
    }
    CsRef m_register_extension(interpreter&, const cs_args& a,
                               const char* kind) {
        const std::string id = pos_str(i, a, 0);
        CsRef meta = pos_or(a, 1, cs_dict());
        CsRef handler = a.pos.size() > 2 ? a.pos[2] : nullptr;
        cb_box* hb = handler ? keep_cb(i, handler) : nullptr;
        const int32_t r = sao_plugins_ctx_register_extension(
            c(), kind, id.c_str(), jstr(i, meta).c_str(),
            hb ? reinterpret_cast<void*>(tr_action) : nullptr, hb);
        return cs_bool(r == 0);
    }
    CsRef m_menu_category(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        const std::string icon = pos_str(i, a, 1);
        CsRef builder = pos_or(a, 2);
        const double priority = pos_num(i, a, 3, 0.0);
        if (name.empty() || name.find('\0') != std::string::npos || name.size() > 1024 ||
            icon.find('\0') != std::string::npos || icon.size() > 256 ||
            !std::isfinite(priority) || !menu_callable(builder))
            i.raise_exc("ArgumentException", "invalid menu category or builder", {});
        auto menu = std::make_unique<menu_state>();
        menu->i = &i;
        menu->builder = builder;
        menu->action_handler = menu_action_handler;
        menu->name = name;
        menu->icon = icon;
        menu->priority = priority;
        const auto hash = [](const std::string& value) {
            uint64_t result = 14695981039346656037ULL;
            for (const unsigned char c : value) { result ^= c; result *= 1099511628211ULL; }
            return std::to_string(result);
        };
        menu->id = "menu-" + hash(name);
        menu->root = "plugin:" + hash(i.cfg.plugin_id + "\n" + name);
        const loader::entity_root_contribution_descriptor root{sizeof(root), menu->id.c_str(),
            menu->root.c_str(), menu->name.c_str(), menu->icon.c_str(), priority};
        const loader::context_entity_provider_descriptor_v3 descriptor{sizeof(descriptor),
            menu->id.c_str(), menu_snapshot, nullptr, menu.get(), &root, menu_action, menu.get(), 0, 0};
        menus.reserve(menus.size() + 1);
        const int32_t status = sao_plugins_ctx_register_entity_provider_v3(c(), &descriptor);
        if (status != SAO_OK)
            i.raise_exc("InvalidOperationException", "register_menu_category failed with status " + std::to_string(status), {});
        menus.push_back(std::move(menu));
        return cs_true();
    }
    CsRef m_menu_surface(interpreter&, const cs_args& a) {
        const std::string sid = pos_str(i, a, 0);
        CsRef desc = pos_or(a, 1, cs_dict());
        const float prio = static_cast<float>(pos_num(i, a, 2, 0.0));
        const int32_t r = sao_plugins_ctx_register_menu_surface(
            c(), sid.c_str(), jstr(i, desc).c_str(), prio);
        return cs_bool(r == 0);
    }
    CsRef m_register_action_handler(interpreter&, const cs_args& a) {
        CsRef cb = pos_or(a, 0);
        if (!cb)
            i.raise_exc("ArgumentException",
                        "register_action_handler() missing handler", {});
        if (!menu_callable(cb)) i.raise_exc("ArgumentException", "action handler is not callable", {});
        *menu_action_handler = cb;
        return cs_true();
    }
    CsRef m_register_engine(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        CsRef eng = pos_or(a, 1);
        dict_set(as_dict(engines), cs_str(name), eng ? eng : cs_null());
        return cs_null();
    }
    CsRef m_get_engine(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        if (CsRef v = dict_get(as_dict(engines), cs_str(name)))
            return cs_is_null(v) ? pos_or(a, 1, cs_null()) : v;
        void* raw = sao_plugins_ctx_get_engine(c(), name.c_str());
        if (!raw)
            return pos_or(a, 1, cs_null());
        auto d = cs_dict();
        dict_set(d, cs_str("__native_engine__"), cs_true());
        dict_set(d, cs_str("name"), cs_str(name));
        dict_set(d, cs_str("handle"),
                 cs_int(reinterpret_cast<int64_t>(raw)));
        return d;
    }
    CsRef m_require_engine(interpreter&, const cs_args& a) {
        CsRef v = m_get_engine(i, a);
        if (cs_is_null(v))
            i.raise_exc("InvalidOperationException",
                        "required engine missing: " + pos_str(i, a, 0), {});
        return v;
    }
    CsRef m_call_engine(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        const std::string method = pos_str(i, a, 1);
        CsRef eng = m_get_engine(i, a);
        if (!cs_is_null(eng) && !as_dict(eng)) {
            CsRef fn = i.getattr(eng, method);
            if (!fn)
                i.raise_exc("MissingMemberException",
                            "engine '" + name + "' has no '" + method + "'",
                            {});
            cs_args ca;
            for (std::size_t k = 2; k < a.pos.size(); ++k)
                ca.pos.push_back(a.pos[k]);
            return i.call(fn, ca, {});
        }
        return cs_null();
    }
    CsRef m_call_runtime(interpreter&, const cs_args&) {
        return cs_null();    // runtime RPC — graceful no-op subset
    }
    // positional form: register_data_source(id, meta, start, stop)
    CsRef m_register_data_source(interpreter&, const cs_args& a) {
        const std::string id = pos_str(i, a, 0);
        CsRef meta = pos_or(a, 1, cs_dict());
        CsRef start = pos_or(a, 2);
        CsRef stop = pos_or(a, 3);
        if (start && start->kind == cs_kind::null_)
            start = nullptr;
        if (stop && stop->kind == cs_kind::null_)
            stop = nullptr;
        cb_box* carrier = keep_cb(i, cs_dict(), id);
        auto* cd = as_dict(carrier->fn);
        if (start)
            dict_set(cd, cs_str("start"), start);
        if (stop)
            dict_set(cd, cs_str("stop"), stop);
        const int32_t r = sao_plugins_ctx_register_data_source(
            c(), id.c_str(), jstr(i, meta).c_str(),
            start ? [](void* ud) -> int32_t {
                auto* b = static_cast<cb_box*>(ud);
                if (!b || !b->i)
                    return 0;
                cs_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn), cs_str("start")),
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
                cs_guard g(*b->i);
                try {
                    b->i->call0(dict_get(as_dict(b->fn), cs_str("stop")),
                                {});
                } catch (...) {
                    return -1;
                }
                return 0;
            }
                 : static_cast<loader::data_source_stop_fn>(nullptr),
            carrier);
        return cs_bool(r == 0);
    }
    CsRef m_register_thread(interpreter&, const cs_args&) {
        return cs_null();    // bookkeeping-only in v1
    }
    CsRef m_ensure_requirements(interpreter&, const cs_args& a) {
        const bool install =
            a.pos.empty() ? true : cs_truthy(a.pos[0]);
        char* raw = nullptr;
        if (sao_plugins_ctx_ensure_requirements(c(), install, &raw) != 0 ||
            !raw)
            return cs_dict();
        CsRef r = parse_json(i, raw);
        free_ctx_str(raw);
        return r;
    }
    CsRef m_load_local(interpreter&, const cs_args& a) {
        if (a.pos.size() != 1 || !as_str(a.pos[0]) ||
            as_str(a.pos[0])->v.find('\0') != std::string::npos)
            i.raise_exc("ArgumentException", "load_local expects one path string without NUL", {});
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
            i.raise_exc("InvalidOperationException", "load_local failed with status " +
                        std::to_string(r) + ": " + diag, {});
        switch (kind) {
        case script::load_local_result::module:
            if (mod)
                return wrap_script_module(i, mod);
            return cs_null();
        case script::load_local_result::path_only:
            return cs_str(narrow_w(abs));
        default:
            if (i.on_log)
                i.on_log("load_local(" + rel + "): " + diag);
            return cs_null();
        }
    }
    CsRef m_open(interpreter&, const cs_args&) {
        // System.IO is outside the subset — ctx.open raises io feature.
        i.record_feature("io");
        i.raise_exc("NotSupportedException",
                    "ctx.open is outside the csmini subset (feature 'io')",
                    {});
    }
    CsRef m_time(interpreter&, const cs_args&) {
        return cs_float(static_cast<double>(
                            std::chrono::system_clock::now()
                                .time_since_epoch()
                                .count()) /
                        static_cast<double>(
                            std::chrono::system_clock::period::den) *
                        static_cast<double>(
                            std::chrono::system_clock::period::num));
    }

    // ── reflective engine surface (binding_engine.h contract) ────────
    // Lazy bind of the owned SaoSdkContext.  Returns nullptr while unbound
    // (probe interpreters without cfg.ctx, or a failed bind).
    const SaoSdkContext* engine_ctx() noexcept {
        if (engine_ctx_ready)
            return &engine_sdk_ctx;
        if (engine_ctx_failed || !i.cfg.ctx)
            return nullptr;
        if (sao_sdk_bind_context(i.cfg.plugin_id.c_str(), "",
                                 &engine_sdk_ctx) != SAO_SDK_OK) {
            engine_ctx_failed = true;
            return nullptr;
        }
        engine_ctx_ready = true;
        // Missing/absent platform services stay as null provider slots —
        // catalog availability probes on those entries fail closed.
        (void)sao_sdk_context_bind_platform_services(&engine_sdk_ctx);
        return &engine_sdk_ctx;
    }
    const SaoSdkContext* need_engine_ctx() {
        const SaoSdkContext* sdk = engine_ctx();
        if (!sdk)
            i.raise_exc("NotSupportedException",
                        "ctx.engine: SDK context is not bound", {});
        return sdk;
    }
    // engine status code → C# exception type (file convention).
    static const char* engine_exc_type(int32_t status) {
        switch (status) {
        case SAO_ERR_INVALID_ARGUMENT:
            return "ArgumentException";
        case SAO_SDK_ERR_UNSUPPORTED:
        case loader::SAO_PLUGINS_ERR_UNSUPPORTED:
        case SAO_ERR_NOT_IMPLEMENTED:
            return "NotSupportedException";
        default:
            return "InvalidOperationException";
        }
    }
    // Generic engine_callback trampoline: resolves channel → keep_cb box
    // registered via engine.on(), then calls cb(channel, payload) under the
    // interpreter guard.  Errors are swallowed (trampoline convention here).
    static void SAO_PLUGINS_CALL
    tr_engine_cb(const char* channel_utf8, const uint8_t* payload_utf8,
                 size_t payload_size, void* user_data) {
        auto* self = static_cast<ctx_builder*>(user_data);
        if (!self || !channel_utf8)
            return;
        loader::context_runtime_lease invocation(self->i.context_lifetime.lock());
        if (self->i.cfg.ctx && !invocation)
            return;
        cs_guard g(self->i);
        cb_box* target = nullptr;
        if (const auto found = self->engine_channels.find(channel_utf8);
            found != self->engine_channels.end())
            target = found->second;
        if (!target || !target->i || !target->fn)
            return;
        try {
            const std::string text(
                reinterpret_cast<const char*>(payload_utf8),
                payload_utf8 ? payload_size : 0u);
            cs_args a2;
            a2.pos.push_back(cs_str(channel_utf8));
            a2.pos.push_back(parse_json(*target->i, text.c_str()));
            (void)target->i->call(target->fn, a2, {});
        } catch (...) {
        }
    }

    // Core invoke: build {"name", "args"[, "callback_channel"]} and call
    // sao_plugins_sdk_context_dispatch(…, method_engine_call, …).  Nonzero
    // dispatch or inner {"status"} codes raise per engine_exc_type().
    CsRef engine_dispatch(const std::string& catalog_name,
                          nlohmann::json fn_args) {
        const SaoSdkContext* sdk = need_engine_ctx();
        if (!fn_args.is_object())
            fn_args = nlohmann::json::object();
        nlohmann::ordered_json envelope;
        envelope["name"] = catalog_name;
        // "callback_channel" is an envelope field, not a function arg.
        if (auto cc = fn_args.find("callback_channel"); cc != fn_args.end()) {
            envelope["callback_channel"] = *cc;
            fn_args.erase(cc);
        }
        envelope["args"] = std::move(fn_args);
        const std::string payload = envelope.dump();

        sdk_binding::sdk_context_call_request request{};
        request.args_json_utf8 = payload.c_str();
        request.args_size = payload.size();
        request.engine_callback = &ctx_builder::tr_engine_cb;
        request.callback_user_data = this;

        size_t required = 0;
        request.out_required = &required;
        std::vector<char> out(256u * 1024u);
        request.out_result_json_utf8 = out.data();
        request.out_capacity = out.size();
        int32_t status = sdk_binding::sao_plugins_sdk_context_dispatch(
            sdk, sdk_binding::sdk_method_id::method_engine_call, &request);
        // One bounded retry when the result outgrew the first buffer.
        if (status == SAO_ERR_BUFFER_TOO_SMALL && required > out.size() &&
            required <= sdk_binding::kMaximumBindingJsonBytes + 1) {
            out.assign(required, '\0');
            request.out_result_json_utf8 = out.data();
            request.out_capacity = out.size();
            required = 0;
            status = sdk_binding::sao_plugins_sdk_context_dispatch(
                sdk, sdk_binding::sdk_method_id::method_engine_call,
                &request);
        }
        if (status != SAO_OK)
            i.raise_exc(engine_exc_type(status),
                        "ctx.engine." + catalog_name +
                            " dispatch failed (status " +
                            std::to_string(status) + ")",
                        {});
        if (out[0] == '\0')
            return cs_null();    // SAO_OK with no result body written
        nlohmann::json result;
        try {
            result = nlohmann::json::parse(out.data());
        } catch (...) {
            i.raise_exc("InvalidOperationException",
                        "ctx.engine." + catalog_name +
                            " returned invalid JSON",
                        {});
        }
        if (!result.is_object())
            return cs_from_json(i, result);
        const int64_t inner = result.value("status", int64_t{0});
        if (inner != 0)
            i.raise_exc(engine_exc_type(static_cast<int32_t>(inner)),
                        "ctx.engine." + catalog_name +
                            " failed (status " + std::to_string(inner) + ")",
                        {});
        if (result.contains("result"))
            return cs_from_json(i, result["result"]);
        return cs_from_json(i, result);
    }

    // ctx.engine_call(name, dict) — raw escape hatch (also engine.invoke).
    CsRef m_engine_call(interpreter&, const cs_args& a) {
        const std::string name = pos_str(i, a, 0);
        CsRef dict_arg = pos_or(a, 1, cs_dict());
        nlohmann::json fn_args = json_from_cs(i, dict_arg);
        if (!fn_args.is_object())
            i.raise_exc("ArgumentException",
                        "engine_call() args must be a dict", {});
        return engine_dispatch(name, std::move(fn_args));
    }

    // One builtin per catalog entry: positional args land on arg_names in
    // declaration order; a trailing dict positional merges {name: value}
    // pairs (kwargs substitute — the C# subset parses `name:` args but
    // carries no named-arg channel).
    CsRef engine_invoke_named(const std::string& catalog_name,
                              const cs_args& a) {
        const sdk_binding::sdk_engine_function_desc* desc =
            sdk_binding::sdk_engine_catalog_find(catalog_name);
        const uint32_t arity = desc ? desc->arg_count : 0;
        nlohmann::ordered_json fn_args = nlohmann::ordered_json::object();
        uint32_t filled = 0;
        for (const CsRef& v : a.pos) {
            if (filled < arity) {
                fn_args[desc->arg_names[filled]] = json_from_cs(i, v);
                ++filled;
            } else if (auto* dv = as_dict(v)) {
                for (const auto& [k, x] : dv->items) {
                    auto* ks = as_str(k);
                    if (!ks)
                        i.raise_exc(
                            "ArgumentException",
                            "ctx.engine." + catalog_name +
                                " kwargs keys must be strings", {});
                    fn_args[ks->v] = json_from_cs(i, x);
                }
            } else {
                i.raise_exc("ArgumentException",
                            "ctx.engine." + catalog_name +
                                ": too many positional arguments", {});
            }
        }
        return engine_dispatch(catalog_name, std::move(fn_args));
    }

    // ctx.engine.list() — runtime catalog [{name, args, available}].
    // `available` needs the lazily bound SDK ctx; unbound → all false.
    CsRef m_engine_list(interpreter&, const cs_args&) {
        const SaoSdkContext* sdk = engine_ctx();
        CsRef out = cs_array();
        const std::size_t n = sdk_binding::sdk_engine_catalog_size();
        for (std::size_t k = 0; k < n; ++k) {
            const sdk_binding::sdk_engine_function_desc* desc =
                sdk_binding::sdk_engine_catalog_at(k);
            if (!desc || !desc->name)
                continue;
            CsRef entry = cs_dict();
            dict_set(entry, cs_str("name"), cs_str(desc->name));
            CsRef names = cs_array();
            for (uint32_t j = 0; j < desc->arg_count; ++j)
                as_array(names)->v.push_back(cs_str(desc->arg_names[j]));
            dict_set(entry, cs_str("args"), names);
            dict_set(
                entry, cs_str("available"),
                cs_bool(sdk != nullptr &&
                        sdk_binding::sdk_engine_entry_available(sdk, desc)));
            as_array(out)->v.push_back(entry);
        }
        return out;
    }

    // ctx.engine.on(channel, cb) — register a generic engine_callback
    // trampoline target; engine calls carrying "callback_channel": <name>
    // deliver payloads to it.  Kept alive via keep_cb / g_cb_keep.
    CsRef m_engine_on(interpreter&, const cs_args& a) {
        const std::string channel = pos_str(i, a, 0);
        CsRef cb = pos_or(a, 1);
        if (channel.empty() || !cb || cb->kind == cs_kind::null_ ||
            (cb->kind != cs_kind::func && cb->kind != cs_kind::builtin &&
             cb->kind != cs_kind::bound_method))
            i.raise_exc("ArgumentException",
                        "engine.on() requires (channel, callback)", {});
        cb_box* b = keep_cb(i, cb);
        engine_channels[channel] = b;
        return cb;
    }

    // ── ui.* spec builder ────────────────────────────────────────────
    CsRef make_ui() {
        auto d = cs_dict();
        std::size_t n_methods = 0;
        const char* const* names = script::script_ui_methods(&n_methods);
        for (std::size_t k = 0; k < n_methods; ++k) {
            const std::string mname = names[k];
            dict_set(d, cs_str(mname),
                     cs_builtin("ui." + mname,
                                [this, mname](interpreter&,
                                              const cs_args& a) {
                                    // C# subset: always positional array req
                                    nlohmann::json req =
                                        nlohmann::json::array();
                                    for (const auto& p : a.pos)
                                        req.push_back(json_from_cs(i, p));
                                    nlohmann::json node;
                                    std::string err;
                                    if (!script::script_ui_build(
                                            mname.c_str(), req, node, err))
                                        i.raise_exc("ArgumentException",
                                                    err, {});
                                    return cs_from_json(i, node);
                                }));
        }
        return d;
    }

    // engine/property sub-objects
    CsRef make_event_bus() {
        auto d = cs_dict();
        dict_set(d, cs_str("subscribe"),
                 method("subscribe",
                        [this](interpreter&, const cs_args& a) {
                            return m_subscribe(i, a);
                        }));
        dict_set(d, cs_str("subscribe_once"),
                 method("subscribe_once",
                        [this](interpreter&, const cs_args& a) {
                            return m_subscribe_once(i, a);
                        }));
        dict_set(d, cs_str("unsubscribe"),
                 method("unsubscribe",
                        [this](interpreter&, const cs_args& a) {
                            return m_unsubscribe(i, a);
                        }));
        dict_set(d, cs_str("emit"),
                 method("emit",
                        [this](interpreter&, const cs_args& a) {
                            return m_emit(i, a);
                        }));
        dict_set(d, cs_str("on"),
                 method("on",
                        [this](interpreter&, const cs_args& a) {
                            return m_on(i, a);
                        }));
        return d;
    }
    CsRef make_mem() {
        // ctx.mem — the reflective engine surface's `mem.*` group exposed
        // with the group prefix stripped: catalog "mem.read_u64" binds as
        // ctx.mem.read_u64(...).  Mirrors the v1 MemAccess facade; a call
        // surfaces the provider status when no memory provider is wired.
        auto d = cs_dict();
        const std::size_t catalog_n = sdk_binding::sdk_engine_catalog_size();
        for (std::size_t k = 0; k < catalog_n; ++k) {
            const sdk_binding::sdk_engine_function_desc* desc =
                sdk_binding::sdk_engine_catalog_at(k);
            if (!desc || !desc->name)
                continue;
            const std::string_view full(desc->name);
            if (full.size() <= 4 || full.substr(0, 4) != "mem.")
                continue;
            const std::string attr(full.substr(4));
            if (attr.empty() || dict_get(d, cs_str(attr)))
                continue;
            script::ctx_surface_note(loader::engine_kind::csharp,
                                     ("mem." + attr).c_str());
            dict_set(d, cs_str(attr),
                     cs_builtin("mem." + attr,
                                [this, cn = std::string(full)](interpreter&,
                                                               const cs_args& a) {
                                    return engine_invoke_named(cn, a);
                                }));
        }
        // CsNativeObj: plain dicts reject `obj.member` access in csmini.
        auto proxy = cs_native("ctx.mem", true);
        as_native(proxy)->members = d;
        return proxy;
    }
    CsRef make_engine_obj() {
        auto d = cs_dict();
        dict_set(d, cs_str("get"),
                 method("engine.get",
                        [this](interpreter&, const cs_args& a) {
                            return m_get_engine(i, a);
                        }));
        dict_set(d, cs_str("call"),
                 method("engine.call",
                        [this](interpreter&, const cs_args& a) {
                            return m_call_engine(i, a);
                        }));
        dict_set(d, cs_str("require"),
                 method("engine.require",
                        [this](interpreter&, const cs_args& a) {
                            return m_require_engine(i, a);
                        }));
        dict_set(d, cs_str("register"),
                 method("engine.register",
                        [this](interpreter&, const cs_args& a) {
                            return m_register_engine(i, a);
                        }));
        // reflective engine surface — one method per sdk_engine catalog
        // entry: catalog "mem.read_u64" → ctx.engine.mem_read_u64.
        const std::size_t catalog_n = sdk_binding::sdk_engine_catalog_size();
        for (std::size_t k = 0; k < catalog_n; ++k) {
            const sdk_binding::sdk_engine_function_desc* desc =
                sdk_binding::sdk_engine_catalog_at(k);
            if (!desc || !desc->name)
                continue;
            std::string mname = desc->name;
            for (auto& ch : mname)
                if (ch == '.')
                    ch = '_';
            script::ctx_surface_note(loader::engine_kind::csharp,
                                     ("engine." + mname).c_str());
            dict_set(d, cs_str(mname),
                     cs_builtin("engine." + mname,
                                [this, cn = std::string(desc->name)](
                                    interpreter&, const cs_args& a) {
                                    return engine_invoke_named(cn, a);
                                }));
        }
        dict_set(d, cs_str("list"),
                 method("engine.list",
                        [this](interpreter&, const cs_args& a) {
                            return m_engine_list(i, a);
                        }));
        dict_set(d, cs_str("on"),
                 method("engine.on",
                        [this](interpreter&, const cs_args& a) {
                            return m_engine_on(i, a);
                        }));
        dict_set(d, cs_str("invoke"),
                 method("engine.invoke",
                        [this](interpreter&, const cs_args& a) {
                            return m_engine_call(i, a);
                        }));
        // wrap as CsNativeObj: plain dicts reject `obj.member` access in
        // csmini; the proxy exposes the members dict as attributes.
        auto proxy = cs_native("ctx.engine", true);
        as_native(proxy)->members = d;
        return proxy;
    }

    // script_module → module proxy (CsNativeObj, members dict)
    CsRef wrap_script_module(
        interpreter&, const std::shared_ptr<script::script_module>& mod) {
        auto proxy = cs_native(mod->module_id(), false);
        auto* pd = as_dict(as_native(proxy)->members);
        for (const auto& mname : mod->member_names()) {
            script::script_value_ptr v;
            std::string err;
            const int32_t status = mod->get(mname, &v, &err);
            if (status != SAO_OK)
                i.raise_exc("InvalidOperationException", "load_local member " + mname +
                            " failed with status " + std::to_string(status) + ": " + err, {});
            if (v && v->k == script::script_value::kind::function) {
                dict_set(pd, cs_str(mname),
                         cs_builtin(
                             mname,
                             [this, mod, mname](interpreter& i2,
                                                const cs_args& a2) {
                                 std::vector<script::script_value_ptr>
                                     args;
                                 for (const auto& p : a2.pos)
                                     args.push_back(cs_to_sv(i2, p));
                                 script::script_value_ptr out;
                                 std::string err2;
                                 if (mod->call(mname, args, &out,
                                               &err2) != 0)
                                     i2.raise_exc(
                                         "InvalidOperationException",
                                         "load_local module call failed: " +
                                             mname + ": " + err2,
                                         {});
                                 return sv_to_cs(i2, out);
                             }));
            } else {
                dict_set(pd, cs_str(mname), sv_to_cs(i, v));
            }
        }
        return proxy;
    }

    // script_value ↔ CsRef marshalling (cross-language boundary)
    script::script_value_ptr cs_to_sv(interpreter&, const CsRef& v) {
        if (!v)
            return script::script_value::null_value();
        switch (v->kind) {
        case cs_kind::null_:
            return script::script_value::null_value();
        case cs_kind::boolean:
            return script::script_value::make_boolean(as_bool(v)->v);
        case cs_kind::integer:
            return script::script_value::make_integer(as_int(v)->v);
        case cs_kind::char_:
            return script::script_value::make_integer(as_char(v)->v);
        case cs_kind::number:
            return script::script_value::make_number(as_float(v)->v);
        case cs_kind::string:
            return script::script_value::make_string(as_str(v)->v);
        case cs_kind::guid:
            return script::script_value::make_string(
                cs_guid_format(as_guid(v)->bytes));
        case cs_kind::array: {
            std::vector<script::script_value_ptr> items;
            for (const auto& x : as_array(v)->v)
                items.push_back(cs_to_sv(i, x));
            return script::script_value::make_list(std::move(items));
        }
        case cs_kind::dict: {
            std::vector<std::pair<std::string, script::script_value_ptr>>
                obj;
            for (const auto& [k, x] : as_dict(v)->items)
                obj.emplace_back(cs_to_str(i, k), cs_to_sv(i, x));
            return script::script_value::make_map(std::move(obj));
        }
        case cs_kind::instance: {
            std::vector<std::pair<std::string, script::script_value_ptr>>
                obj;
            for (const auto& [k, x] : as_dict(as_inst(v)->attrs)->items)
                if (auto* ks = as_str(k))
                    obj.emplace_back(ks->v, cs_to_sv(i, x));
            return script::script_value::make_map(std::move(obj));
        }
        default:
            return script::script_value::make_string(cs_to_str(i, v));
        }
    }
    CsRef sv_to_cs(interpreter&, const script::script_value_ptr& v) {
        if (!v)
            return cs_null();
        using k = script::script_value::kind;
        switch (v->k) {
        case k::null:
            return cs_null();
        case k::boolean:
            return cs_bool(v->boolean);
        case k::integer:
            return cs_int(v->integer);
        case k::number:
            return cs_float(v->number);
        case k::string:
            return cs_str(v->text);
        case k::list: {
            auto items = cs_array();
            for (const auto& x : v->items)
                as_array(items)->v.push_back(sv_to_cs(i, x));
            return items;
        }
        case k::map: {
            auto d = cs_dict();
            for (const auto& [k2, x] : v->object)
                dict_set(d, cs_str(k2), sv_to_cs(i, x));
            return d;
        }
        case k::function:
            if (!v->call)
                return cs_null();
            return cs_builtin(
                "cross_fn",
                [this, v](interpreter& i2, const cs_args& a2) {
                    std::vector<script::script_value_ptr> args;
                    for (const auto& p : a2.pos)
                        args.push_back(cs_to_sv(i2, p));
                    script::script_value_ptr out;
                    std::string err;
                    if (v->call(args, &out, &err) != 0)
                        i2.raise_exc("InvalidOperationException",
                                     "cross-language call failed: " + err,
                                     {});
                    return sv_to_cs(i2, out);
                });
        default:
            return cs_null();
        }
    }

    // ── install all methods into a CsNativeObj ("ctx", ci) ───────────
    CsRef build() {
        auto proxy = cs_native("ctx", true);    // case-insensitive members
        auto* d = as_dict(as_native(proxy)->members);
        auto put = [&](const char* n, cs_native_fn f) {
            dict_set(d, cs_str(n), method(n, std::move(f)));
        };
        auto putv = [&](const char* n, CsRef v) {
            dict_set(d, cs_str(n), std::move(v));
        };

        // properties
        putv("plugin_id", cs_str(i.cfg.plugin_id));
        putv("path", cs_str(root_utf8));
        putv("web_path", cs_str(root_utf8 + "/web"));
        putv("assets_path", cs_str(root_utf8 + "/assets"));
        putv("should_stop",
             cs_builtin("should_stop",
                        [this](interpreter&, const cs_args&) {
                            return cs_bool(sao_plugins_ctx_should_stop(c()));
                        }));
        putv("owner", cs_str(i.cfg.plugin_id));

        // events + props
        put("log", [this](interpreter&, const cs_args& a) { return m_log(i, a); });
        put("subscribe", [this](interpreter&, const cs_args& a) { return m_subscribe(i, a); });
        put("subscribe_once", [this](interpreter&, const cs_args& a) { return m_subscribe_once(i, a); });
        put("unsubscribe", [this](interpreter&, const cs_args& a) { return m_unsubscribe(i, a); });
        put("on", [this](interpreter&, const cs_args& a) { return m_on(i, a); });
        put("on_damage", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "damage"); });
        put("on_heal", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "heal"); });
        put("on_skill", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "skill"); });
        put("on_boss", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "boss"); });
        put("on_snapshot", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "snapshot"); });
        put("on_encounter_finalized", [this](interpreter&, const cs_args& a) { return on_topic(i, a, "encounter_finalized"); });
        put("emit", [this](interpreter&, const cs_args& a) { return m_emit(i, a); });
        put("get_snapshot", [this](interpreter&, const cs_args& a) { return m_get_snapshot(i, a); });
        put("snapshot_value", [this](interpreter&, const cs_args& a) { return m_snapshot_value(i, a); });
        put("recent_events", [this](interpreter&, const cs_args& a) { return m_recent_events(i, a); });

        // settings
        put("get_setting", [this](interpreter&, const cs_args& a) { return m_get_setting(i, a); });
        put("setting", [this](interpreter&, const cs_args& a) { return m_get_setting(i, a); });
        put("set_setting", [this](interpreter&, const cs_args& a) { return m_set_setting(i, a); });
        put("set_defaults", [this](interpreter&, const cs_args& a) { return m_set_defaults(i, a); });

        // registrations
        put("register_parser_adapter", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "parser_adapter"); });
        put("register_exporter", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "exporter"); });
        put("register_formatter", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "formatter"); });
        put("register_trigger_type", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "trigger_type"); });
        put("register_report_view", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "report_view"); });
        put("register_timer", [this](interpreter&, const cs_args& a) { return m_register_extension(i, a, "timer_ext"); });
        put("register_ui_panel", [this](interpreter&, const cs_args& a) { return m_register_ui_panel(i, a); });
        put("register_render_hook", [this](interpreter&, const cs_args& a) { return m_render_hook(i, a); });
        put("unregister_render_hook", [this](interpreter&, const cs_args& a) { return m_unregister_render_hook(i, a); });
        put("set_overlay", [this](interpreter&, const cs_args& a) { return m_set_overlay(i, a); });
        put("clear_overlay", [this](interpreter&, const cs_args& a) { return m_clear_overlay(i, a); });
        put("request_redraw", [this](interpreter&, const cs_args& a) { return m_request_redraw(i, a); });
        put("register_hotkey", [this](interpreter&, const cs_args& a) { return m_register_hotkey(i, a); });
        put("unregister_hotkey", [this](interpreter&, const cs_args& a) { return m_unregister_hotkey(i, a); });
        put("register_menu_category", [this](interpreter&, const cs_args& a) { return m_menu_category(i, a); });
        put("register_menu_surface", [this](interpreter&, const cs_args& a) { return m_menu_surface(i, a); });
        put("register_action_handler", [this](interpreter&, const cs_args& a) { return m_register_action_handler(i, a); });
        put("register_engine", [this](interpreter&, const cs_args& a) { return m_register_engine(i, a); });
        put("register_data_source", [this](interpreter&, const cs_args& a) { return m_register_data_source(i, a); });
        put("register_thread", [this](interpreter&, const cs_args& a) { return m_register_thread(i, a); });

        // timers
        put("set_interval", [this](interpreter&, const cs_args& a) { return m_set_interval(i, a); });
        put("set_timeout", [this](interpreter&, const cs_args& a) { return m_set_timeout(i, a); });
        put("clear_timer", [this](interpreter&, const cs_args& a) { return m_clear_timer(i, a); });
        put("complete_timer", [this](interpreter&, const cs_args& a) { return m_complete_timer(i, a); });
        put("run_on_ui", [this](interpreter&, const cs_args& a) { return m_run_on_ui(i, a); });

        // notify/dialog
        put("notify", [this](interpreter&, const cs_args& a) { return m_notify(i, a); });
        put("dismiss_notify", [this](interpreter&, const cs_args& a) { return m_dismiss_notify(i, a); });
        put("toast", [this](interpreter&, const cs_args& a) { return m_toast(i, a); });
        put("open_file", [this](interpreter&, const cs_args& a) { return m_open_file(i, a); });
        put("open_window", [this](interpreter&, const cs_args& a) { return m_open_window(i, a); });

        // compositor
        put("create_compositor_layer", [this](interpreter&, const cs_args& a) { return m_create_layer(i, a); });
        put("upload_compositor_frame", [this](interpreter&, const cs_args& a) { return m_upload_frame(i, a); });
        put("set_compositor_layer_mmf_source", [this](interpreter&, const cs_args& a) { return m_layer_mmf(i, a); });
        put("set_compositor_layer_shared_texture_source", [this](interpreter&, const cs_args& a) { return m_layer_shared_tex(i, a); });
        put("set_compositor_layer_position", [this](interpreter&, const cs_args& a) { return m_layer_pos(i, a); });
        put("set_compositor_layer_visible", [this](interpreter&, const cs_args& a) { return m_layer_visible(i, a); });
        put("set_compositor_layer_input", [this](interpreter&, const cs_args& a) { return m_layer_input(i, a); });
        put("destroy_compositor_layer", [this](interpreter&, const cs_args& a) { return m_layer_destroy(i, a); });
        put("compositor_gpu_interop_available", [this](interpreter&, const cs_args& a) { return m_gpu_interop(i, a); });
        put("compositor_layer_shared_texture_active", [this](interpreter&, const cs_args& a) { return m_layer_shared_tex_active(i, a); });
        put("compositor_display_refresh_hz", [this](interpreter&, const cs_args& a) { return m_layer_refresh(i, a); });

        // engine/deps
        put("get_engine", [this](interpreter&, const cs_args& a) { return m_get_engine(i, a); });
        put("require_engine", [this](interpreter&, const cs_args& a) { return m_require_engine(i, a); });
        put("call_engine", [this](interpreter&, const cs_args& a) { return m_call_engine(i, a); });
        put("engine_call", [this](interpreter&, const cs_args& a) { return m_engine_call(i, a); });
        put("call_runtime", [this](interpreter&, const cs_args& a) { return m_call_runtime(i, a); });
        put("ensure_requirements", [this](interpreter&, const cs_args& a) { return m_ensure_requirements(i, a); });
        put("load_local", [this](interpreter&, const cs_args& a) { return m_load_local(i, a); });
        put("open", [this](interpreter&, const cs_args& a) { return m_open(i, a); });
        put("time", [this](interpreter&, const cs_args& a) { return m_time(i, a); });

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
    "set_compositor_layer_mmf_source", "set_compositor_layer_shared_texture_source",
    "set_compositor_layer_position", "set_compositor_layer_visible",
    "set_compositor_layer_input", "destroy_compositor_layer",
    "compositor_gpu_interop_available",
    "compositor_layer_shared_texture_active",
    "compositor_display_refresh_hz",
    "get_engine", "require_engine", "call_engine", "engine_call",
    "call_runtime",
    "ensure_requirements", "load_local", "open", "time",
    "ui", "event_bus", "engine", "mem",
    // reflective engine surface statics (catalog entries are noted per
    // bound name as "engine.<mname>" inside make_engine_obj)
    "engine.list", "engine.on", "engine.invoke",
    // ui.* names recorded under their canonical spellings
    "ui.panel", "ui.section", "ui.card", "ui.row", "ui.group", "ui.text",
    "ui.title", "ui.kv", "ui.bar", "ui.slider", "ui.badge", "ui.divider",
    "ui.spacer", "ui.button", "ui.input", "ui.table", "ui.canvas",
    "ui.rgba_frame", "ui.rect", "ui.oval", "ui.line", "ui.ctext",
    nullptr,
};

} // namespace

// ── public entry: build ctx object bound to interpreter cfg ───────────
CsRef csmini_make_ctx(interpreter& i) {
    loader::context_runtime_lease invocation(i.cfg.ctx);
    if (i.cfg.ctx && !invocation)
        i.raise_exc("InvalidOperationException", "plugin context is retired", {});
    i.context_lifetime = invocation.state();
    // heap-allocated + anchored: method lambdas in build() capture the raw
    // `this`, so the builder must outlive the ctx dict.
    auto b = std::make_shared<ctx_builder>(i);
    {
        std::lock_guard<std::mutex> g(g_cb_mu);
        g_ctx_builders[&i].push_back(b);
    }
    ctx_builder& br = *b;
    // ledgers scaffold (pyhost parity)
    auto* ld = as_dict(br.ledgers);
    dict_set(ld, cs_str("panels"), cs_dict());
    dict_set(ld, cs_str("hotkeys"), cs_dict());
    dict_set(ld, cs_str("timers"), cs_dict());
    dict_set(ld, cs_str("render_hooks"), cs_dict());
    dict_set(ld, cs_str("extensions"), cs_dict());
    br.root_utf8 = narrow_w(i.cfg.plugin_root);
    script::ctx_surface_note_all(loader::engine_kind::csharp,
                                 k_surface_names);
    return br.build();
}

} // namespace sao::plugins::csmini
