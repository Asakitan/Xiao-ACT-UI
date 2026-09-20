// script_ui.cpp — `ctx.ui.*` builder mirror of act_platform/ui_spec.py::UI.
//
// Emits the *builder* dicts (pre-normalization).  Scalar truncation rules
// (_s/_ci/_clamp01/_cpos/_cz) are applied here exactly where the Python
// builders applied them; everything else (node-kind filters, budgets) is left
// to `sao_engine_ui_spec_normalize` downstream.
#include "sao/plugins/script_ctx/script_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace sao::plugins::script_ctx {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxText = 4000;
constexpr std::size_t kMaxTitle = 200;
constexpr int kMaxLayerPos = 32768;
constexpr int kMaxLayerZ = 10000;
constexpr int kMaxCanvasDim = 4096;
constexpr std::size_t kMaxInputVal = 2000;

// _s: stringify (json scalars → UTF-8), truncate with ellipsis like Python.
std::string s_clip(const json& v, std::size_t limit = kMaxText) {
    std::string text;
    if (v.is_null()) {
    } else if (v.is_string()) {
        text = v.get<std::string>();
    } else if (v.is_boolean()) {
        text = v.get<bool>() ? "True" : "False";
    } else if (v.is_number()) {
        if (v.is_number_float()) {
            const double d = v.get<double>();
            const double r = std::round(d);
            if (d == r && std::abs(d) < 1e15)
                text = std::to_string(static_cast<long long>(r));
            else {
                text = std::to_string(d);
                while (text.size() > 1 && text.back() == '0')
                    text.pop_back();
                if (!text.empty() && text.back() == '.')
                    text += '0';
            }
        } else {
            text = std::to_string(v.get<long long>());
        }
    } else {
        text = v.dump();
    }
    if (text.size() > limit) {
        text.resize(limit > 0 ? limit - 1 : 0);
        text += "…";
    }
    return text;
}

double num(const json& v, double def = 0.0) noexcept {
    if (v.is_number())
        return v.get<double>();
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return def;
}

double clamp01(const json& v) noexcept {
    const double d = num(v, 0.0);
    if (std::isnan(d))
        return 0.0;
    return std::max(0.0, std::min(1.0, d));
}

int cint(const json& v, int def = 0) noexcept {
    const double d = num(v, static_cast<double>(def));
    if (std::isnan(d))
        return def;
    return static_cast<int>(std::lround(d));
}

int cpos(const json& v, int def = 0) noexcept {
    return std::max(-kMaxLayerPos, std::min(kMaxLayerPos, cint(v, def)));
}

int cz(const json& v, int def = 0) noexcept {
    return std::max(-kMaxLayerZ, std::min(kMaxLayerZ, cint(v, def)));
}

json arr_or_empty(const json& v) {
    if (v.is_array())
        return v;
    return json::array();
}

// Argument resolution: positional array or kwargs object.
struct arg_view {
    const json* pos = nullptr;   // array args
    const json* kw = nullptr;    // object args
    std::size_t pos_count = 0;

    const json& get(std::size_t index, const char* name,
                    const json& fallback) const noexcept {
        if (pos != nullptr && index < pos_count)
            return (*pos)[index];
        if (kw != nullptr) {
            const auto it = kw->find(name);
            if (it != kw->end())
                return *it;
        }
        return fallback;
    }
};

const json& null_json() {
    static const json v;
    return v;
}

using method_fn = json (*)(const arg_view&);

std::string lower_copy(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// ── node builders (1:1 mirror of ui_spec.py UI methods) ────────────────────

json m_panel(const arg_view& a) {
    return {{"type", "panel"},
            {"title", s_clip(a.get(0, "title", null_json()), kMaxTitle)},
            {"children", arr_or_empty(a.get(1, "children", null_json()))}};
}

json m_section(const arg_view& a) {
    return {{"type", "section"},
            {"title", s_clip(a.get(0, "title", null_json()), kMaxTitle)},
            {"accent", s_clip(a.get(2, "accent", null_json()).is_null()
                              ? json("cyan")
                              : a.get(2, "accent", null_json()), 40)},
            {"children", arr_or_empty(a.get(1, "children", null_json()))}};
}

json m_card(const arg_view& a) {
    return {{"type", "card"},
            {"title", s_clip(a.get(0, "title", null_json()), kMaxTitle)},
            {"children", arr_or_empty(a.get(1, "children", null_json()))}};
}

json m_row(const arg_view& a) {
    const json& align = a.get(1, "align", null_json());
    return {{"type", "row"},
            {"align", align.is_null() ? "left" : s_clip(align, 40)},
            {"children", arr_or_empty(a.get(0, "children", null_json()))}};
}

json m_group(const arg_view& a) {
    return {{"type", "group"},
            {"children", arr_or_empty(a.get(0, "children", null_json()))}};
}

json m_text(const arg_view& a) {
    const json& style = a.get(1, "style", null_json());
    const json& align = a.get(2, "align", null_json());
    return {{"type", "text"},
            {"text", s_clip(a.get(0, "text", null_json()))},
            {"style", style.is_null() ? "value" : s_clip(style, 40)},
            {"align", align.is_null() ? "left" : s_clip(align, 40)}};
}

json m_title(const arg_view& a) {
    return {{"type", "text"},
            {"text", s_clip(a.get(0, "text", null_json()))},
            {"style", "title"},
            {"align", "left"}};
}

json m_kv(const arg_view& a) {
    const json& style = a.get(2, "style", null_json());
    return {{"type", "kv"},
            {"label", s_clip(a.get(0, "label", null_json()), 200)},
            {"value", s_clip(a.get(1, "value", null_json()), 400)},
            {"style", style.is_null() ? "value" : s_clip(style, 40)}};
}

json m_bar(const arg_view& a) {
    const json& color = a.get(2, "color", null_json());
    return {{"type", "bar"},
            {"label", s_clip(a.get(0, "label", null_json()), 200)},
            {"pct", clamp01(a.get(1, "pct", null_json()))},
            {"color", color.is_null() ? "cyan" : s_clip(color, 40)},
            {"caption", s_clip(a.get(3, "caption", null_json()), 200)}};
}

json m_slider(const arg_view& a) {
    const json& color = a.get(6, "color", null_json());
    return {{"type", "slider"},
            {"label", s_clip(a.get(0, "label", null_json()), 200)},
            {"id", s_clip(a.get(1, "id", null_json()), 80)},
            {"value", num(a.get(2, "value", null_json()), 0.0)},
            {"lo", num(a.get(3, "lo", null_json()), 0.0)},
            {"hi", num(a.get(4, "hi", null_json()), 1.0)},
            {"step", num(a.get(5, "step", null_json()), 0.01)},
            {"color", color.is_null() ? "cyan" : s_clip(color, 40)}};
}

json m_badge(const arg_view& a) {
    const json& style = a.get(1, "style", null_json());
    return {{"type", "badge"},
            {"text", s_clip(a.get(0, "text", null_json()), 120)},
            {"style", style.is_null() ? "muted" : s_clip(style, 40)}};
}

json m_divider(const arg_view&) {
    return {{"type", "divider"}};
}

json m_spacer(const arg_view& a) {
    const json& sz = a.get(0, "size", null_json());
    return {{"type", "spacer"}, {"size", sz.is_null() ? 8 : cint(sz, 8)}};
}

json m_button(const arg_view& a) {
    json node{{"type", "button"},
              {"label", s_clip(a.get(0, "label", null_json()), 120)},
              {"action", s_clip(a.get(1, "action", null_json()), 120)}};
    const json& style = a.get(2, "style", null_json());
    node["style"] = style.is_null() ? "default" : s_clip(style, 40);
    const json& payload = a.get(3, "payload", null_json());
    if (payload.is_object() && !payload.empty())
        node["payload"] = payload;
    const json& disabled = a.get(4, "disabled", null_json());
    if (disabled.is_boolean() && disabled.get<bool>())
        node["disabled"] = true;
    else if (disabled.is_number() && disabled.get<double>() != 0.0)
        node["disabled"] = true;
    return node;
}

json m_input(const arg_view& a) {
    return {{"type", "input"},
            {"id", s_clip(a.get(0, "id", null_json()), 80)},
            {"value", s_clip(a.get(1, "value", null_json()), kMaxInputVal)},
            {"placeholder", s_clip(a.get(2, "placeholder", null_json()), 200)},
            {"input_type", s_clip(a.get(3, "input_type", null_json()).is_null()
                                   ? json("text")
                                   : a.get(3, "input_type", null_json()), 40)},
            {"width", std::max(0, std::min(2000, cint(a.get(4, "width", null_json()), 0)))}};
}

json m_table(const arg_view& a) {
    return {{"type", "table"},
            {"columns", arr_or_empty(a.get(0, "columns", null_json()))},
            {"rows", arr_or_empty(a.get(1, "rows", null_json()))},
            {"highlight_key", s_clip(a.get(2, "highlight_key", null_json()), 80)},
            {"title", s_clip(a.get(3, "title", null_json()), kMaxTitle)}};
}

json m_canvas(const arg_view& a) {
    const json& bg = a.get(3, "bg", null_json());
    const json& draggable = a.get(8, "draggable", null_json());
    return {{"type", "canvas"},
            {"id", s_clip(a.get(7, "id", null_json()), 120)},
            {"x", cpos(a.get(4, "x", null_json()), 0)},
            {"y", cpos(a.get(5, "y", null_json()), 0)},
            {"z", cz(a.get(6, "z", null_json()), 0)},
            {"width", std::max(1, std::min(kMaxCanvasDim,
                                           cint(a.get(0, "width", null_json()), 320)))},
            {"height", std::max(1, std::min(kMaxCanvasDim,
                                            cint(a.get(1, "height", null_json()), 160)))},
            {"draggable", draggable.is_boolean() && draggable.get<bool>()},
            {"bg", bg.is_null() ? "body" : s_clip(bg, 40)},
            {"ops", arr_or_empty(a.get(2, "ops", null_json()))}};
}

json m_rgba_frame(const arg_view& a) {
    const int w = std::max(1, std::min(kMaxCanvasDim,
                                     cint(a.get(1, "width", null_json()), 320)));
    const int h = std::max(1, std::min(kMaxCanvasDim,
                                       cint(a.get(2, "height", null_json()), 480)));
    const json& draggable = a.get(7, "draggable", null_json());
    const json& hit_test = a.get(9, "hit_test", null_json());
    const json& premultiplied = a.get(10, "premultiplied", null_json());
    return {{"type", "rgba_frame"},
            {"id", s_clip(a.get(0, "id", null_json()), 120)},
            {"x", cpos(a.get(4, "x", null_json()), 0)},
            {"y", cpos(a.get(5, "y", null_json()), 0)},
            {"z", cz(a.get(6, "z", null_json()), 0)},
            {"width", w},
            {"height", h},
            {"draggable", !draggable.is_boolean() || draggable.get<bool>()},
            {"hit_test", hit_test.is_null() ? "alpha" : s_clip(hit_test, 40)},
            {"frame_rgba_b64", s_clip(a.get(3, "frame_rgba_b64", null_json()),
                                      static_cast<std::size_t>(w) * h * 8)},
            {"frame_key", s_clip(a.get(8, "frame_key", null_json()), 240)},
            {"premultiplied", premultiplied.is_boolean() && premultiplied.get<bool>()}};
}

json canvas_shape(const arg_view& a, const char* op) {
    return {{"op", op},
            {"x", a.get(0, "x", null_json())},
            {"y", a.get(1, "y", null_json())},
            {"w", a.get(2, "w", null_json())},
            {"h", a.get(3, "h", null_json())},
            {"fill", a.get(4, "fill", null_json()).is_null()
                         ? json("")
                         : json(s_clip(a.get(4, "fill", null_json()), 40))},
            {"outline", a.get(5, "outline", null_json()).is_null()
                            ? json("")
                            : json(s_clip(a.get(5, "outline", null_json()), 40))},
            {"width", cint(a.get(6, "width", null_json()), 0)}};
}

json m_rect(const arg_view& a) {
    return canvas_shape(a, "rect");
}

json m_oval(const arg_view& a) {
    return canvas_shape(a, "oval");
}

json m_line(const arg_view& a) {
    const json& fill = a.get(4, "fill", null_json());
    return {{"op", "line"},
            {"x1", a.get(0, "x1", null_json())},
            {"y1", a.get(1, "y1", null_json())},
            {"x2", a.get(2, "x2", null_json())},
            {"y2", a.get(3, "y2", null_json())},
            {"fill", fill.is_null() ? "value" : s_clip(fill, 40)},
            {"width", cint(a.get(5, "width", null_json()), 1)}};
}

json m_ctext(const arg_view& a) {
    const json& fill = a.get(3, "fill", null_json());
    const json& anchor = a.get(5, "anchor", null_json());
    const json& bold = a.get(6, "bold", null_json());
    return {{"op", "text"},
            {"x", a.get(0, "x", null_json())},
            {"y", a.get(1, "y", null_json())},
            {"text", s_clip(a.get(2, "text", null_json()), 200)},
            {"fill", fill.is_null() ? "value" : s_clip(fill, 40)},
            {"size", cint(a.get(4, "size", null_json()), 10)},
            {"anchor", anchor.is_null() ? "nw" : s_clip(anchor, 40)},
            {"bold", bold.is_boolean() && bold.get<bool>()}};
}

struct method_entry {
    const char* name;
    method_fn fn;
};

constexpr std::array kMethods = {
    method_entry{"badge", &m_badge},
    method_entry{"bar", &m_bar},
    method_entry{"button", &m_button},
    method_entry{"canvas", &m_canvas},
    method_entry{"card", &m_card},
    method_entry{"ctext", &m_ctext},
    method_entry{"divider", &m_divider},
    method_entry{"group", &m_group},
    method_entry{"input", &m_input},
    method_entry{"kv", &m_kv},
    method_entry{"line", &m_line},
    method_entry{"oval", &m_oval},
    method_entry{"panel", &m_panel},
    method_entry{"rect", &m_rect},
    method_entry{"rgba_frame", &m_rgba_frame},
    method_entry{"row", &m_row},
    method_entry{"section", &m_section},
    method_entry{"spacer", &m_spacer},
    method_entry{"slider", &m_slider},
    method_entry{"table", &m_table},
    method_entry{"text", &m_text},
    method_entry{"title", &m_title},
};

} // namespace

bool script_ui_build(const char* method,
                     const nlohmann::json& args,
                     nlohmann::json& out_node,
                     std::string& out_error) noexcept {
    try {
        if (method == nullptr || *method == '\0') {
            out_error = "missing ui method name";
            return false;
        }
        std::string name = method;
        if (name.rfind("ui.", 0) == 0)
            name.erase(0, 3);
        name = lower_copy(name);
        method_fn fn = nullptr;
        for (const auto& entry : kMethods) {
            if (name == entry.name) {
                fn = entry.fn;
                break;
            }
        }
        if (fn == nullptr) {
            out_error = "unknown ctx.ui method: " + name;
            return false;
        }
        arg_view view{};
        if (args.is_array()) {
            view.pos = &args;
            view.pos_count = args.size();
        } else if (args.is_object()) {
            view.kw = &args;
        } else if (!args.is_null()) {
            out_error = "ui." + name + " args must be an array or object";
            return false;
        }
        out_node = fn(view);
        return true;
    } catch (const std::exception& e) {
        out_error = std::string("ui builder error: ") + e.what();
        return false;
    } catch (...) {
        out_error = "ui builder error";
        return false;
    }
}

const char* const* script_ui_methods(std::size_t* out_count) noexcept {
    if (out_count != nullptr)
        *out_count = kMethods.size();
    static const char* const names[] = {
        "badge", "bar", "button", "canvas", "card", "ctext", "divider",
        "group", "input", "kv", "line", "oval", "panel", "rect",
        "rgba_frame", "row", "section", "spacer", "slider", "table",
        "text", "title",
    };
    return names;
}

} // namespace sao::plugins::script_ctx
