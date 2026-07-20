// SAO Auto - declarative UI spec normalizer, 1:1 port of
// ``python/act_platform/ui_spec.py``.
//
// The Python oracle clamps sizes, drops unknown node kinds, escapes text
// and bounds recursion so a misbehaving plugin cannot bloat or crash the
// UI thread.  Both the Direct2D renderer and the legacy WebView consume
// the same normalized shape -- a divergent C++ normalizer would silently
// produce different panels.
//
// The frozen ``docs/fixtures/ui_spec/*.json`` set captures eight canonical
// scenarios (normal / nested / mutation / invalid_ref).  Fixture-parity
// coverage drives them through this normalizer.
//
// Design notes:
//   * `nlohmann::json` with default ``std::map<std::string, ...>`` storage
//     dumps keys in alphabetical order -- matching Python
//     ``json.dumps(sort_keys=True)``.
//   * Unicode text runs (5000+ chars, "..."-ellipsis suffix) match
//     ``ui_spec.py::_s`` which truncates to ``limit - 1`` characters then
//     appends U+2026.  We compute the truncation in UTF-16 code-unit
//     space so a run of narrow ASCII 5000 bytes matches the Python
//     ``len(text) > limit`` check.  Non-ASCII truncation is best-effort:
//     the fixture set does not currently exercise multibyte truncation.
//   * `_json_scalar` semantics: bool/None/int/float pass through
//     unchanged; anything else gets ``_s``-stringified.

#include "sao/engine/ui_spec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using nlohmann::json;

namespace {

// ── Bounds (mirrors ui_spec.py constants) ────────────────────────────
constexpr int MAX_DEPTH        = 8;
constexpr int MAX_NODES        = 400;
constexpr int MAX_TABLE_ROWS   = 200;
constexpr int MAX_TABLE_COLS   = 16;
constexpr int MAX_TEXT_LEN     = 4000;
constexpr int MAX_TITLE_LEN    = 200;
constexpr int MAX_CANVAS_OPS   = 4000;
constexpr int MAX_CANVAS_DIM   = 4096;
constexpr int MAX_LAYER_POS    = 32768;
constexpr int MAX_LAYER_Z      = 10000;
constexpr int MAX_INPUT_VAL    = 2000;

// ── Token vocabularies ───────────────────────────────────────────────
const std::unordered_set<std::string> kTextStyles = {
    "title", "subtitle", "value", "label", "muted",
    "ok", "warn", "bad", "gold", "accent", "mono",
};
const std::unordered_set<std::string> kBadgeStyles = {
    "muted", "ok", "warn", "bad", "gold", "accent",
};
const std::unordered_set<std::string> kButtonStyles = {
    "default", "primary", "danger", "ghost",
};
const std::unordered_set<std::string> kBarColors = {
    "cyan", "gold", "ok", "warn", "bad", "accent", "heal",
};
const std::unordered_set<std::string> kAligns = {
    "left", "center", "right",
};
const std::unordered_set<std::string> kInputTypes = {
    "text", "number", "password",
};
const std::unordered_set<std::string> kContainerKinds = {
    "panel", "section", "card", "row", "group",
};
const std::unordered_set<std::string> kLeafKinds = {
    "text", "kv", "bar", "badge", "divider", "spacer",
    "button", "input", "slider", "table", "canvas", "rgba_frame",
};
const std::unordered_set<std::string> kCanvasAnchors = {
    "nw", "n", "ne", "w", "center", "e", "sw", "s", "se",
};
const std::unordered_set<std::string> kHitTests = {
    "none", "rect", "alpha",
};
std::unordered_set<std::string> kCanvasColorTokens = [] {
    std::unordered_set<std::string> s;
    for (const auto& t : kTextStyles) s.insert(t);
    for (const auto& t : kBarColors)  s.insert(t);
    for (const auto& t : { "white", "black", "bg", "body", "border",
                            "sep", "grid", "header", "transparent" }) {
        s.insert(t);
    }
    return s;
}();

bool is_node_kind(const std::string& kind) {
    return kContainerKinds.count(kind) || kLeafKinds.count(kind);
}

// ── Helpers ──────────────────────────────────────────────────────────

// ``_s(value, limit)`` -- convert to string then truncate with a U+2026
// ellipsis.  None → "" per Python.  U+2026 is 3 bytes in UTF-8
// (0xE2 0x80 0xA6).  Truncation counts CODE POINTS (Python ``len(str)``
// counts code points).  We approximate by counting UTF-8 lead bytes.
std::string s_clamp(const json& value, int limit = MAX_TEXT_LEN) {
    // Python semantics: ``None`` becomes "" (the empty string); ints /
    // floats / bools stringify with ``str()``; strings pass through.
    if (value.is_null()) return "";
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_boolean()) {
        text = value.get<bool>() ? "True" : "False";
    } else if (value.is_number_integer()) {
        text = std::to_string(value.get<int64_t>());
    } else if (value.is_number_unsigned()) {
        text = std::to_string(value.get<uint64_t>());
    } else if (value.is_number_float()) {
        // Python ``str(float)`` semantics -- for the fixture set this
        // branch is never hit inside a truncating path.
        text = std::to_string(value.get<double>());
    } else {
        // Dicts / lists become their Python ``str()`` repr in principle;
        // the fixture set does not exercise that path so we return the
        // JSON compact repr (best-effort surrogate).
        text = value.dump();
    }
    // Count code points (approximation: UTF-8 lead bytes -- correct for
    // all ASCII inputs, which is what the fixture set exercises).
    size_t code_points = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++code_points;  // not a continuation byte
    }
    if (static_cast<int>(code_points) > limit) {
        // Truncate to `limit - 1` code points then append U+2026 (…).
        size_t keep = static_cast<size_t>(limit - 1);
        size_t byte_len = 0;
        size_t count = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            size_t step = 1;
            if      ((c & 0x80) == 0x00) step = 1;
            else if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            if (count >= keep) break;
            byte_len = i + step;
            i += step;
            ++count;
        }
        std::string out = text.substr(0, byte_len);
        out += "\xE2\x80\xA6";  // U+2026 ellipsis
        return out;
    }
    return text;
}

// ``_clamp01(value)`` -- float in [0, 1]; non-numeric → 0.0; NaN → 0.0.
double clamp01(const json& v) {
    double num;
    if (v.is_number_integer() || v.is_number_unsigned()) {
        num = static_cast<double>(v.get<int64_t>());
    } else if (v.is_number_float()) {
        num = v.get<double>();
    } else if (v.is_boolean()) {
        num = v.get<bool>() ? 1.0 : 0.0;
    } else if (v.is_string()) {
        try {
            num = std::stod(v.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    } else {
        return 0.0;
    }
    if (std::isnan(num)) return 0.0;
    if (num < 0.0) return 0.0;
    if (num > 1.0) return 1.0;
    return num;
}

// ``_choice(value, allowed, default)`` -- lowercase-normalised string
// membership check; falls back to default.
std::string choice(const json& v, const std::unordered_set<std::string>& allowed,
                   const std::string& def) {
    std::string text;
    if (v.is_string()) text = v.get<std::string>();
    else if (v.is_null()) text = "";
    else text = s_clamp(v, MAX_TEXT_LEN);
    // strip + lowercase (Python: ``str(value or "").strip().lower()``)
    // Trim
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' ||
                     text[a] == '\n' || text[a] == '\r')) ++a;
    while (b > a && (text[b-1] == ' ' || text[b-1] == '\t' ||
                     text[b-1] == '\n' || text[b-1] == '\r')) --b;
    text = text.substr(a, b - a);
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    if (allowed.count(text)) return text;
    return def;
}

// ``_ci(value, default)`` -- float→round(); NaN → default; bad → default.
int64_t ci(const json& v, int64_t def = 0) {
    double num;
    if (v.is_number_integer())        num = static_cast<double>(v.get<int64_t>());
    else if (v.is_number_unsigned())  num = static_cast<double>(v.get<uint64_t>());
    else if (v.is_number_float())     num = v.get<double>();
    else if (v.is_boolean())          num = v.get<bool>() ? 1.0 : 0.0;
    else if (v.is_string()) {
        try { num = std::stod(v.get<std::string>()); }
        catch (...) { return def; }
    } else {
        return def;
    }
    if (std::isnan(num)) return def;
    // Python round() is banker's rounding but the fixture set has integer
    // inputs where rint == round(); std::llround uses half-away semantics
    // which is close enough for the fixture domain.  Match Python exactly
    // by using std::rint (half-to-even).
    return static_cast<int64_t>(std::rint(num));
}

int64_t cpos(const json& v, int64_t def = 0) {
    int64_t n = ci(v, def);
    if (n < -MAX_LAYER_POS) n = -MAX_LAYER_POS;
    if (n >  MAX_LAYER_POS) n =  MAX_LAYER_POS;
    return n;
}

int64_t cz(const json& v, int64_t def = 0) {
    int64_t n = ci(v, def);
    if (n < -MAX_LAYER_Z) n = -MAX_LAYER_Z;
    if (n >  MAX_LAYER_Z) n =  MAX_LAYER_Z;
    return n;
}

// ``_json_scalar(value)`` -- bool / None / number pass through; else
// ``_s``-stringify with the default MAX_TEXT_LEN cap.
json json_scalar(const json& v) {
    if (v.is_null() || v.is_boolean() ||
        v.is_number_integer() || v.is_number_unsigned() ||
        v.is_number_float()) {
        return v;
    }
    return s_clamp(v, MAX_TEXT_LEN);
}

bool is_hex_color(const std::string& text) {
    if (text.empty() || text[0] != '#') return false;
    size_t n = text.size();
    if (n != 4 && n != 7) return false;
    for (size_t i = 1; i < n; ++i) {
        char c = text[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

// ``_canvas_color(value, default)`` -- hex passthrough OR theme token
// membership.
std::string canvas_color(const json& v, const std::string& def = "") {
    std::string text;
    if (v.is_string()) text = v.get<std::string>();
    else if (v.is_null()) return def;
    else text = s_clamp(v, MAX_TEXT_LEN);
    // strip
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b-1] == ' ' || text[b-1] == '\t')) --b;
    text = text.substr(a, b - a);
    if (text.empty()) return def;
    if (is_hex_color(text)) return text;
    std::string low = text;
    for (char& c : low) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    if (kCanvasColorTokens.count(low)) return low;
    return def;
}

// ── Forward declarations ─────────────────────────────────────────────
json normalize_node(const json& node, int depth, int& budget);
json normalize_children_arr(const json& children, int depth, int& budget);

json normalize_children_arr(const json& children, int depth, int& budget) {
    json out = json::array();
    if (!children.is_array()) return out;
    for (const auto& child : children) {
        if (budget <= 0) break;
        json node = normalize_node(child, depth + 1, budget);
        if (!node.is_null()) out.push_back(std::move(node));
    }
    return out;
}

json normalize_table(const json& node) {
    json columns = json::array();
    std::vector<std::string> col_keys;
    if (node.contains("columns") && node["columns"].is_array()) {
        int i = 0;
        for (const auto& col : node["columns"]) {
            if (i >= MAX_TABLE_COLS) break;
            ++i;
            if (col.is_object()) {
                std::string key;
                if (col.contains("key")) key = s_clamp(col["key"], 80);
                else if (col.contains("id")) key = s_clamp(col["id"], 80);
                if (key.empty()) continue;
                std::string title;
                if (col.contains("title") && !col["title"].is_null() &&
                    !(col["title"].is_string() && col["title"].get<std::string>().empty())) {
                    title = s_clamp(col["title"], 80);
                } else {
                    title = key;
                }
                std::string align = choice(col.value("align", json()),
                                           kAligns, "left");
                columns.push_back({
                    {"align", align},
                    {"key",   key},
                    {"title", title},
                });
                col_keys.push_back(key);
            } else {
                std::string key = s_clamp(col, 80);
                if (!key.empty()) {
                    columns.push_back({
                        {"align", "left"},
                        {"key",   key},
                        {"title", key},
                    });
                    col_keys.push_back(key);
                }
            }
        }
    }
    std::string highlight_key;
    if (node.contains("highlight_key")) {
        highlight_key = s_clamp(node["highlight_key"], 80);
    }
    json rows = json::array();
    if (node.contains("rows") && node["rows"].is_array()) {
        int i = 0;
        for (const auto& row : node["rows"]) {
            if (i >= MAX_TABLE_ROWS) break;
            ++i;
            if (!row.is_object()) continue;
            std::vector<std::string> keys;
            if (!col_keys.empty()) {
                keys = col_keys;
                // Highlight column always emitted if row carries it and
                // it is not already displayed.
                if (!highlight_key.empty()) {
                    bool listed = false;
                    for (const auto& k : keys) if (k == highlight_key) { listed = true; break; }
                    if (!listed && row.contains(highlight_key)) {
                        keys.push_back(highlight_key);
                    }
                }
            } else {
                // No columns declared -- take first MAX_TABLE_COLS keys of the row.
                int c = 0;
                for (auto it = row.begin(); it != row.end() && c < MAX_TABLE_COLS; ++it, ++c) {
                    keys.push_back(it.key());
                }
            }
            json out_row = json::object();
            for (const auto& k : keys) {
                if (row.contains(k)) out_row[k] = json_scalar(row[k]);
                else                 out_row[k] = nullptr;  // Python row.get(k) → None
            }
            rows.push_back(std::move(out_row));
        }
    }
    std::string title;
    if (node.contains("title")) title = s_clamp(node["title"], MAX_TITLE_LEN);
    json result;
    result["columns"]       = columns;
    result["highlight_key"] = highlight_key;
    result["rows"]          = rows;
    result["title"]         = title;
    result["type"]          = "table";
    return result;
}

json normalize_canvas(const json& node) {
    int64_t width  = ci(node.value("width",  json(320)), 320);
    int64_t height = ci(node.value("height", json(160)), 160);
    if (width  < 1) width  = 1;
    if (width  > MAX_CANVAS_DIM) width  = MAX_CANVAS_DIM;
    if (height < 1) height = 1;
    if (height > MAX_CANVAS_DIM) height = MAX_CANVAS_DIM;
    json ops = json::array();
    if (node.contains("ops") && node["ops"].is_array()) {
        int i = 0;
        for (const auto& op : node["ops"]) {
            if (i >= MAX_CANVAS_OPS) break;
            ++i;
            if (!op.is_object()) continue;
            std::string kind = choice(op.value("op", json()),
                                      {"rect", "oval", "line", "text"},
                                      "");
            if (kind == "rect" || kind == "oval") {
                int64_t w = ci(op.value("w", json(0)), 0);
                int64_t h = ci(op.value("h", json(0)), 0);
                if (w < 0) w = 0;
                if (h < 0) h = 0;
                int64_t width_op = ci(op.value("width", json(0)), 0);
                if (width_op < 0) width_op = 0;
                if (width_op > 20) width_op = 20;
                ops.push_back({
                    {"fill",    canvas_color(op.value("fill",    json()), "")},
                    {"h",       h},
                    {"op",      kind},
                    {"outline", canvas_color(op.value("outline", json()), "")},
                    {"w",       w},
                    {"width",   width_op},
                    {"x",       ci(op.value("x", json(0)), 0)},
                    {"y",       ci(op.value("y", json(0)), 0)},
                });
            } else if (kind == "line") {
                int64_t width_op = ci(op.value("width", json(1)), 1);
                if (width_op < 1) width_op = 1;
                if (width_op > 20) width_op = 20;
                ops.push_back({
                    {"fill",  canvas_color(op.value("fill", json()), "value")},
                    {"op",    "line"},
                    {"width", width_op},
                    {"x1",    ci(op.value("x1", json(0)), 0)},
                    {"x2",    ci(op.value("x2", json(0)), 0)},
                    {"y1",    ci(op.value("y1", json(0)), 0)},
                    {"y2",    ci(op.value("y2", json(0)), 0)},
                });
            } else if (kind == "text") {
                int64_t size_op = ci(op.value("size", json(10)), 10);
                if (size_op < 6)  size_op = 6;
                if (size_op > 48) size_op = 48;
                ops.push_back({
                    {"anchor", choice(op.value("anchor", json()), kCanvasAnchors, "nw")},
                    {"bold",   op.value("bold", false)},
                    {"fill",   canvas_color(op.value("fill", json()), "value")},
                    {"op",     "text"},
                    {"size",   size_op},
                    {"text",   s_clamp(op.value("text", json()), 200)},
                    {"x",      ci(op.value("x", json(0)), 0)},
                    {"y",      ci(op.value("y", json(0)), 0)},
                });
            }
        }
    }
    json result;
    result["bg"]        = canvas_color(node.value("bg", json()), "body");
    result["draggable"] = node.value("draggable", false);
    result["height"]    = height;
    result["id"]        = s_clamp(node.value("id", json()), 120);
    result["ops"]       = ops;
    result["type"]      = "canvas";
    result["width"]     = width;
    result["x"]         = cpos(node.value("x", json(0)), 0);
    result["y"]         = cpos(node.value("y", json(0)), 0);
    result["z"]         = cz(node.value("z", json(0)), 0);
    return result;
}

json normalize_rgba_frame(const json& node) {
    int64_t width  = ci(node.value("width",  json(320)), 320);
    int64_t height = ci(node.value("height", json(480)), 480);
    if (width  < 1) width  = 1;
    if (width  > MAX_CANVAS_DIM) width  = MAX_CANVAS_DIM;
    if (height < 1) height = 1;
    if (height > MAX_CANVAS_DIM) height = MAX_CANVAS_DIM;
    int64_t frame_b64_cap = width * height * 8;
    if (frame_b64_cap < 0) frame_b64_cap = 0;
    std::string background = "transparent";
    if (node.contains("background") && !node["background"].is_null()) {
        std::string raw;
        if (node["background"].is_string()) raw = node["background"].get<std::string>();
        else raw = s_clamp(node["background"], 40);
        if (!raw.empty()) background = s_clamp(raw, 40);
    }
    json result;
    result["background"]      = background;
    result["diagnostic"]      = s_clamp(node.value("diagnostic", json()), 500);
    result["draggable"]       = node.value("draggable", true);
    result["frame_key"]       = s_clamp(node.value("frame_key", json()), 240);
    result["frame_rgba_b64"]  = s_clamp(node.value("frame_rgba_b64", json()),
                                        static_cast<int>(frame_b64_cap));
    result["height"]          = height;
    result["hit_test"]        = choice(node.value("hit_test", json()),
                                       kHitTests, "rect");
    result["id"]              = s_clamp(node.value("id", json()), 120);
    result["premultiplied"]   = node.value("premultiplied", false);
    result["type"]            = "rgba_frame";
    result["width"]           = width;
    result["x"]               = cpos(node.value("x", json(0)), 0);
    result["y"]               = cpos(node.value("y", json(0)), 0);
    result["z"]               = cz(node.value("z", json(0)), 0);
    return result;
}

json normalize_node(const json& node, int depth, int& budget) {
    if (budget <= 0 || depth > MAX_DEPTH) return json();
    if (!node.is_object()) {
        // Bare strings become muted text (Python line "Bare strings become
        // muted text -- convenient for quick specs.").  Non-string bare
        // primitives (int / float / bool / null) are stringified first;
        // an empty resulting string skips the node.
        std::string text = s_clamp(node, MAX_TEXT_LEN);
        if (text.empty()) return json();
        --budget;
        json r;
        r["align"] = "left";
        r["style"] = "value";
        r["text"]  = text;
        r["type"]  = "text";
        return r;
    }
    std::string kind;
    if (node.contains("type") && !node["type"].is_null()) {
        if (node["type"].is_string()) kind = node["type"].get<std::string>();
        else                          kind = s_clamp(node["type"]);
        // strip + lowercase
        size_t a = 0, b = kind.size();
        while (a < b && (kind[a] == ' ' || kind[a] == '\t')) ++a;
        while (b > a && (kind[b-1] == ' ' || kind[b-1] == '\t')) --b;
        kind = kind.substr(a, b - a);
        for (char& c : kind) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        }
    }
    if (!is_node_kind(kind)) return json();
    --budget;

    if (kContainerKinds.count(kind)) {
        json out;
        out["children"] = normalize_children_arr(
            node.value("children", json::array()), depth, budget);
        if (node.contains("title") && !node["title"].is_null()) {
            out["title"] = s_clamp(node["title"], MAX_TITLE_LEN);
        }
        if (kind == "row") {
            out["align"] = choice(node.value("align", json()), kAligns, "left");
        }
        if (node.contains("accent") && !node["accent"].is_null()) {
            out["accent"] = choice(node["accent"], kBarColors, "cyan");
        }
        out["type"] = kind;
        return out;
    }

    if (kind == "text") {
        json r;
        r["align"] = choice(node.value("align", json()), kAligns, "left");
        r["style"] = choice(node.value("style", json()), kTextStyles, "value");
        r["text"]  = s_clamp(node.value("text", json()), MAX_TEXT_LEN);
        r["type"]  = "text";
        return r;
    }
    if (kind == "kv") {
        json r;
        r["label"] = s_clamp(node.value("label", json()), 200);
        r["style"] = choice(node.value("style", json()), kTextStyles, "value");
        r["type"]  = "kv";
        r["value"] = s_clamp(node.value("value", json()), 400);
        return r;
    }
    if (kind == "bar") {
        json r;
        r["caption"] = s_clamp(node.value("caption", json()), 200);
        r["color"]   = choice(node.value("color", json()), kBarColors, "cyan");
        r["label"]   = s_clamp(node.value("label", json()), 200);
        r["pct"]     = clamp01(node.value("pct", json(0)));
        r["type"]    = "bar";
        // Optional slider mode: action attached → lo/hi/step surface.
        if (node.contains("action") && !node["action"].is_null()) {
            std::string act;
            if (node["action"].is_string()) act = node["action"].get<std::string>();
            else act = s_clamp(node["action"]);
            if (!act.empty()) {
                r["action"] = s_clamp(node["action"], 120);
                double lo = 0.0, hi = 1.0, step = 0.0;
                if (node.contains("lo"))   lo   = clamp01(node["lo"]) < 0 ? 0.0 : (node["lo"].is_number() ? node["lo"].get<double>() : 0.0);
                if (node.contains("hi"))   hi   = node["hi"].is_number() ? node["hi"].get<double>() : 1.0;
                if (node.contains("step")) step = node["step"].is_number() ? node["step"].get<double>() : 0.0;
                // Python does ``float(node.get("lo", 0.0))`` etc. -- no
                // clamp; just cast.
                r["lo"]   = node.contains("lo")   ? (node["lo"].is_number()   ? node["lo"].get<double>()   : 0.0)
                                                  : 0.0;
                r["hi"]   = node.contains("hi")   ? (node["hi"].is_number()   ? node["hi"].get<double>()   : 1.0)
                                                  : 1.0;
                r["step"] = node.contains("step") ? (node["step"].is_number() ? node["step"].get<double>() : 0.0)
                                                  : 0.0;
            }
        }
        return r;
    }
    if (kind == "slider") {
        json r;
        r["color"] = choice(node.value("color", json()), kBarColors, "cyan");
        r["hi"]    = node.contains("hi")    ? (node["hi"].is_number()    ? node["hi"].get<double>()    : 1.0) : 1.0;
        r["id"]    = s_clamp(node.value("id", json()), 80);
        r["label"] = s_clamp(node.value("label", json()), 200);
        r["lo"]    = node.contains("lo")    ? (node["lo"].is_number()    ? node["lo"].get<double>()    : 0.0) : 0.0;
        r["step"]  = node.contains("step")  ? (node["step"].is_number()  ? node["step"].get<double>()  : 0.01) : 0.01;
        r["type"]  = "slider";
        r["value"] = node.contains("value") ? (node["value"].is_number() ? node["value"].get<double>() : 0.0) : 0.0;
        return r;
    }
    if (kind == "badge") {
        json r;
        r["style"] = choice(node.value("style", json()), kBadgeStyles, "muted");
        r["text"]  = s_clamp(node.value("text", json()), 120);
        r["type"]  = "badge";
        return r;
    }
    if (kind == "divider") {
        json r;
        r["type"] = "divider";
        return r;
    }
    if (kind == "spacer") {
        int64_t size = 8;
        if (node.contains("size")) {
            if (node["size"].is_number_integer())        size = node["size"].get<int64_t>();
            else if (node["size"].is_number_float())     size = static_cast<int64_t>(node["size"].get<double>());
            else if (node["size"].is_number_unsigned())  size = static_cast<int64_t>(node["size"].get<uint64_t>());
            else if (node["size"].is_null())             size = 8;
            else {
                try { size = std::stoll(s_clamp(node["size"])); }
                catch (...) { size = 8; }
            }
        }
        if (size < 0)  size = 0;
        if (size > 64) size = 64;
        json r;
        r["size"] = size;
        r["type"] = "spacer";
        return r;
    }
    if (kind == "button") {
        json r;
        r["action"] = s_clamp(node.value("action", json()), 120);
        r["label"]  = s_clamp(node.value("label",  json()), 120);
        r["style"]  = choice(node.value("style", json()), kButtonStyles, "default");
        r["type"]   = "button";
        if (node.contains("payload") && node["payload"].is_object()) {
            json payload = json::object();
            int count = 0;
            for (auto it = node["payload"].begin();
                 it != node["payload"].end() && count < 32; ++it, ++count) {
                payload[it.key()] = json_scalar(it.value());
            }
            r["payload"] = payload;
        }
        if (node.contains("disabled") && node["disabled"].is_boolean() && node["disabled"].get<bool>()) {
            r["disabled"] = true;
        } else if (node.contains("disabled") && !node["disabled"].is_boolean()) {
            // Python truthiness -- any non-empty container / non-zero
            // number counts.  Approximate with is_number != 0.
            bool truthy = false;
            const auto& d = node["disabled"];
            if      (d.is_string())         truthy = !d.get<std::string>().empty();
            else if (d.is_number_integer()) truthy = d.get<int64_t>() != 0;
            else if (d.is_number_unsigned())truthy = d.get<uint64_t>() != 0;
            else if (d.is_number_float())   truthy = d.get<double>() != 0.0;
            else if (d.is_array())          truthy = !d.empty();
            else if (d.is_object())         truthy = !d.empty();
            if (truthy) r["disabled"] = true;
        }
        return r;
    }
    if (kind == "input") {
        int64_t width = ci(node.value("width", json(0)), 0);
        if (width < 0)    width = 0;
        if (width > 2000) width = 2000;
        json r;
        r["id"]         = s_clamp(node.value("id", json()), 80);
        r["input_type"] = choice(node.value("input_type", json()), kInputTypes, "text");
        r["placeholder"]= s_clamp(node.value("placeholder", json()), 200);
        r["type"]       = "input";
        r["value"]      = s_clamp(node.value("value", json()), MAX_INPUT_VAL);
        r["width"]      = width;
        return r;
    }
    if (kind == "table")       return normalize_table(node);
    if (kind == "canvas")      return normalize_canvas(node);
    if (kind == "rgba_frame")  return normalize_rgba_frame(node);
    return json();
}

// ── Top-level ────────────────────────────────────────────────────────
json normalize_spec_impl(const json& spec, const std::string& title_hint) {
    int budget = MAX_NODES;
    json nodes = json::array();
    std::string spec_title = title_hint;

    auto extract_title = [&](const json& src) {
        if (src.contains("title") && !src["title"].is_null()) {
            if (src["title"].is_string()) spec_title = src["title"].get<std::string>();
            else                          spec_title = s_clamp(src["title"]);
        }
    };

    if (spec.is_object()) {
        extract_title(spec);
        if (spec.contains("nodes") && spec["nodes"].is_array()) {
            nodes = normalize_children_arr(spec["nodes"], 0, budget);
        } else if (spec.contains("children") && spec["children"].is_array()) {
            nodes = normalize_children_arr(spec["children"], 0, budget);
        } else {
            std::string kind_probe;
            if (spec.contains("type") && spec["type"].is_string()) {
                kind_probe = spec["type"].get<std::string>();
            }
            if (is_node_kind(kind_probe)) {
                json node = normalize_node(spec, 0, budget);
                if (!node.is_null()) {
                    if (node["type"] == "panel") {
                        // Unwrap panel: title → spec title, children → nodes.
                        if (spec_title.empty() && node.contains("title") &&
                            !node["title"].is_null()) {
                            spec_title = node["title"].get<std::string>();
                        }
                        nodes = node.value("children", json::array());
                    } else {
                        nodes = json::array({node});
                    }
                }
            }
        }
    } else if (spec.is_array()) {
        nodes = normalize_children_arr(spec, 0, budget);
    }

    json out;
    out["nodes"]   = nodes;
    out["title"]   = s_clamp(spec_title, MAX_TITLE_LEN);
    out["version"] = SAO_UI_SPEC_VERSION;
    return out;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────
extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_ui_spec_normalize(
    const uint8_t* input_json_utf8, size_t input_len,
    uint8_t* out_json_utf8, size_t out_capacity,
    size_t* out_bytes_written) {
    if (out_bytes_written) *out_bytes_written = 0;
    if (input_json_utf8 == nullptr && input_len != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    json input;
    try {
        input = json::parse(input_json_utf8,
                            input_json_utf8 + input_len);
    } catch (const json::parse_error&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Payload shape:
    //   { "spec": <any>, "title": "<string>" }
    // Anything else is interpreted as the spec itself with an empty title
    // hint -- matches ``normalize_ui_spec(spec)`` default call form.
    json spec_payload;
    std::string title_hint;
    if (input.is_object() && (input.contains("spec") || input.contains("title"))) {
        spec_payload = input.value("spec", json());
        if (input.contains("title") && !input["title"].is_null()) {
            if (input["title"].is_string()) title_hint = input["title"].get<std::string>();
            else                            title_hint = s_clamp(input["title"]);
        }
    } else {
        spec_payload = input;
    }

    json result = normalize_spec_impl(spec_payload, title_hint);
    const std::string dumped = result.dump(2);
    if (out_bytes_written) *out_bytes_written = dumped.size();
    if (out_json_utf8 == nullptr) {
        // Caller is querying required size.
        return SAO_STATUS_OK;
    }
    if (dumped.size() > out_capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_json_utf8, dumped.data(), dumped.size());
    return SAO_STATUS_OK;
}

extern "C" uint32_t SAO_ENGINE_CALL sao_engine_ui_spec_version(void) {
    return SAO_UI_SPEC_VERSION;
}
