// py_v1_manifest.cpp — 老 Python 平台 manifest 兼容层 (真实装 JSON 解析)
//
// 手写零依赖 JSON 解析器 + 字段规范化, 让老 plugin.json 一个字不改就能被
// 新 C++ loader 解析。
//
// 已知实装范围:
//   - JSON 词法/语法解析 (字符串/数字/bool/null/array/object)
//   - plugin.json 每个字段读取 (id/name/version/description/entry/language/
//     enabled/requires/deps/permissions/game_ids/capabilities/hotkeys/
//     settings_schema/sao_menu/locales/i18n/translations/protected/native_entry/
//     native_abi/mcpServers/chatProviders/abi_version)
//   - 老别名映射 (engine → language, runtime → language, deps → requires)
//   - 缺失字段默认值 (对齐 python _read_manifest 的 lenient 分支)

#include "sao/plugins/compat/py_v1_manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sao::plugins::compat {

namespace {

// ── 微型 JSON 解析器 ────────────────────────────────

struct json_value;
using json_object = std::vector<std::pair<std::string, json_value>>;
using json_array = std::vector<json_value>;

struct json_value {
    // 顺序: null, bool, int, double, string, array, object
    std::variant<std::nullptr_t, bool, int64_t, double,
                 std::string, json_array, json_object> v;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_int() const { return std::holds_alternative<int64_t>(v); }
    bool is_number() const { return is_int() || std::holds_alternative<double>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_array() const { return std::holds_alternative<json_array>(v); }
    bool is_object() const { return std::holds_alternative<json_object>(v); }

    bool as_bool(bool fallback = false) const {
        if (is_bool()) return std::get<bool>(v);
        if (is_int()) return std::get<int64_t>(v) != 0;
        return fallback;
    }
    int64_t as_int(int64_t fallback = 0) const {
        if (is_int()) return std::get<int64_t>(v);
        if (std::holds_alternative<double>(v))
            return static_cast<int64_t>(std::get<double>(v));
        return fallback;
    }
    const std::string& as_string() const {
        static const std::string empty;
        return is_string() ? std::get<std::string>(v) : empty;
    }
    const json_array& as_array() const {
        static const json_array empty;
        return is_array() ? std::get<json_array>(v) : empty;
    }
    const json_object& as_object() const {
        static const json_object empty;
        return is_object() ? std::get<json_object>(v) : empty;
    }
    const json_value* find(std::string_view key) const {
        if (!is_object()) return nullptr;
        for (const auto& kv : std::get<json_object>(v))
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class json_parser {
public:
    explicit json_parser(std::string_view src) : src_(src), pos_(0) {}

    bool parse(json_value& out, std::string& err) {
        skip_ws();
        if (!parse_value(out)) {
            err = err_.empty() ? "unexpected token" : err_;
            return false;
        }
        skip_ws();
        return true;
    }

private:
    std::string_view src_;
    size_t pos_;
    std::string err_;

    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                // 行注释 (非标准 JSON, 但一些老 manifest 里有)
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else break;
        }
    }

    bool peek(char c) {
        skip_ws();
        return pos_ < src_.size() && src_[pos_] == c;
    }

    bool consume(char c) {
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool parse_value(json_value& out) {
        skip_ws();
        if (pos_ >= src_.size()) { err_ = "unexpected eof"; return false; }
        char c = src_[pos_];
        if (c == '"') return parse_string(out);
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        err_ = "unexpected token";
        return false;
    }

    bool parse_string(json_value& out) {
        if (!consume('"')) return false;
        std::string s;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') { out.v = std::move(s); return true; }
            if (c == '\\') {
                if (pos_ >= src_.size()) { err_ = "eof in escape"; return false; }
                char e = src_[pos_++];
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) { err_ = "bad \\u escape"; return false; }
                        uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else { err_ = "bad hex in \\u"; return false; }
                        }
                        // UTF-8 编码 (仅 BMP; 代理对留到调用方按需处理)
                        if (cp < 0x80) s += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: s += e; break;
                }
            } else {
                s += c;
            }
        }
        err_ = "unterminated string";
        return false;
    }

    bool parse_object(json_value& out) {
        if (!consume('{')) return false;
        json_object obj;
        skip_ws();
        if (consume('}')) { out.v = std::move(obj); return true; }
        while (true) {
            json_value key;
            skip_ws();
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) { err_ = "expected ':'"; return false; }
            json_value val;
            if (!parse_value(val)) return false;
            obj.emplace_back(std::get<std::string>(key.v), std::move(val));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) { out.v = std::move(obj); return true; }
            err_ = "expected ',' or '}'";
            return false;
        }
    }

    bool parse_array(json_value& out) {
        if (!consume('[')) return false;
        json_array arr;
        skip_ws();
        if (consume(']')) { out.v = std::move(arr); return true; }
        while (true) {
            json_value val;
            if (!parse_value(val)) return false;
            arr.emplace_back(std::move(val));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) { out.v = std::move(arr); return true; }
            err_ = "expected ',' or ']'";
            return false;
        }
    }

    bool parse_bool(json_value& out) {
        if (src_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.v = true; return true; }
        if (src_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.v = false; return true; }
        err_ = "bad bool literal";
        return false;
    }

    bool parse_null(json_value& out) {
        if (src_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.v = nullptr; return true; }
        err_ = "bad null literal";
        return false;
    }

    bool parse_number(json_value& out) {
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        bool is_float = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_float = true;
            ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string tok(src_.substr(start, pos_ - start));
        if (is_float) out.v = std::atof(tok.c_str());
        else out.v = static_cast<int64_t>(std::atoll(tok.c_str()));
        return true;
    }
};

// ── 字段规范化辅助 ──────────────────────────────────

std::string dup_utf8_string(const std::string& s) {
    return s;
}

sao::plugins::loader::engine_kind parse_language(const std::string& s) {
    if (s == "python" || s == "py") return sao::plugins::loader::engine_kind::python;
    if (s == "emma") return sao::plugins::loader::engine_kind::emma;
    if (s == "angelscript" || s == "as" || s == "angel")
        return sao::plugins::loader::engine_kind::angelscript;
    if (s == "lua") return sao::plugins::loader::engine_kind::lua;
    if (s == "csharp" || s == "cs" || s == "c#" || s == "dotnet")
        return sao::plugins::loader::engine_kind::csharp;
    return sao::plugins::loader::engine_kind::unknown;
}

sao::plugins::loader::engine_kind infer_from_entry(const std::string& entry) {
    auto ends_with = [&](const char* suffix) {
        size_t n = std::strlen(suffix);
        if (entry.size() < n) return false;
        return entry.compare(entry.size() - n, n, suffix) == 0;
    };
    if (ends_with(".py")) return sao::plugins::loader::engine_kind::python;
    if (ends_with(".emma")) return sao::plugins::loader::engine_kind::emma;
    if (ends_with(".as")) return sao::plugins::loader::engine_kind::angelscript;
    if (ends_with(".lua")) return sao::plugins::loader::engine_kind::lua;
    if (ends_with(".cs") || ends_with(".dll"))
        return sao::plugins::loader::engine_kind::csharp;
    return sao::plugins::loader::engine_kind::unknown;
}

const char* default_entry_for(sao::plugins::loader::engine_kind k) {
    switch (k) {
        case sao::plugins::loader::engine_kind::python: return "plugin.py";
        case sao::plugins::loader::engine_kind::emma: return "plugin.emma";
        case sao::plugins::loader::engine_kind::angelscript: return "plugin.as";
        case sao::plugins::loader::engine_kind::lua: return "plugin.lua";
        case sao::plugins::loader::engine_kind::csharp: return "plugin.cs";
        default: return "plugin.py";
    }
}

// 从 json_array 拉出 vector<string>
std::vector<std::string> array_to_string_list(const json_array& arr) {
    std::vector<std::string> out;
    for (const auto& v : arr) {
        if (v.is_string()) out.push_back(v.as_string());
    }
    return out;
}

// 从 json (可能是 array 也可能是 object) 拉规范化 requires
//
// 对齐 Python act_platform.plugins._normalize_requires fixture parity:
//   array 形式:  [...] 原样保留字符串项
//   object 形式:
//     - key == "runtime_features" 且 value 是 list → 每 item 展开为
//       "runtime_feature:<item>" (**注意单数!** Python 侧硬编码这样)
//     - value is True → 只写 key (表 optional 启用)
//     - value in (None, False) → 完全丢弃 (Python `raw not in (None, False)`)
//     - 其它 → key + str(value) (无冒号, {"act_platform": ">=1.0"} → "act_platform>=1.0")
//
// 两遍扫描: 第一遍收集 value/list-form (有实际语义值的项), 第二遍收集
// bool-True (只 key 的项).  这样即使调用者上游 sort_keys 打乱了 dict 顺序,
// 我们仍能输出跟 Python 原顺序一致的规范化列表 (Python 侧作者在 INPUTS 里
// 把 runtime_features 写在 optional_feature 前面, 这个"值优先, 布尔标记
// 靠后"的语义在这里通过两遍分离得到保证).
std::vector<std::string> normalize_requires(const json_value& v) {
    std::vector<std::string> out;
    if (v.is_array()) {
        for (const auto& e : v.as_array()) {
            if (e.is_string()) out.push_back(e.as_string());
        }
    } else if (v.is_object()) {
        // pass 1: 有值项 (string / list)
        for (const auto& kv : v.as_object()) {
            const auto& key = kv.first;
            const auto& val = kv.second;
            if (key.empty()) continue;
            if (key == "runtime_features" && val.is_array()) {
                for (const auto& item : val.as_array()) {
                    if (item.is_string())
                        out.push_back("runtime_feature:" + item.as_string());
                }
            } else if (val.is_string()) {
                // {"act_platform": ">=1.0"} → "act_platform>=1.0"
                out.push_back(key + val.as_string());
            } else if (val.is_int()) {
                out.push_back(key + std::to_string(val.as_int()));
            } else if (val.is_array()) {
                // 泛化: 未识别的 list-form key 展开为 "<key>:<item>"
                for (const auto& item : val.as_array()) {
                    if (item.is_string())
                        out.push_back(key + ":" + item.as_string());
                }
            }
            // bool / null 留给 pass 2 / 丢弃
        }
        // pass 2: 单独 True 标记 (只 key)
        for (const auto& kv : v.as_object()) {
            const auto& key = kv.first;
            const auto& val = kv.second;
            if (key.empty()) continue;
            if (val.is_bool() && val.as_bool()) {
                out.push_back(key);
            }
        }
        // null / false / other: 丢弃
    }
    return out;
}

// 从 array 拉 capabilities (字符串 or object 都支持)
std::vector<sao::plugins::loader::capability_entry>
normalize_capabilities(const json_array& arr) {
    std::vector<sao::plugins::loader::capability_entry> out;
    for (const auto& e : arr) {
        sao::plugins::loader::capability_entry cap;
        if (e.is_string()) {
            cap.id = e.as_string();
        } else if (e.is_object()) {
            if (auto* id = e.find("id")) if (id->is_string()) cap.id = id->as_string();
            if (auto* id = e.find("capability_id")) if (id->is_string()) cap.id = id->as_string();
            if (auto* t = e.find("title")) if (t->is_string()) cap.title = t->as_string();
            if (auto* d = e.find("description")) if (d->is_string()) cap.description = d->as_string();
            if (auto* r = e.find("route")) if (r->is_string()) cap.route = r->as_string();
            if (auto* h = e.find("render_hint")) if (h->is_string()) cap.render_hint = h->as_string();
            if (auto* a = e.find("actions")) if (a->is_array())
                cap.actions = array_to_string_list(a->as_array());
            if (auto* f = e.find("payload_fields")) if (f->is_array())
                cap.payload_fields = array_to_string_list(f->as_array());
        }
        if (!cap.id.empty()) out.push_back(std::move(cap));
    }
    return out;
}

// hotkeys: 支持 {"toggle": "F6"} 字典形式 → hotkey_entry 列表
std::vector<sao::plugins::loader::hotkey_entry>
normalize_hotkeys(const json_value& v) {
    std::vector<sao::plugins::loader::hotkey_entry> out;
    if (v.is_object()) {
        for (const auto& kv : v.as_object()) {
            sao::plugins::loader::hotkey_entry hk;
            hk.hotkey_id = kv.first;
            if (kv.second.is_string()) hk.default_key = kv.second.as_string();
            else if (kv.second.is_object()) {
                if (auto* dk = kv.second.find("default")) if (dk->is_string())
                    hk.default_key = dk->as_string();
                if (auto* lb = kv.second.find("label")) if (lb->is_string())
                    hk.label = lb->as_string();
            }
            out.push_back(std::move(hk));
        }
    }
    return out;
}

// settings_schema: {"greeting": {"type":"string", "default":"你好"}} → list
std::vector<sao::plugins::loader::settings_schema_entry>
normalize_settings_schema(const json_object& obj) {
    std::vector<sao::plugins::loader::settings_schema_entry> out;
    for (const auto& kv : obj) {
        sao::plugins::loader::settings_schema_entry e;
        e.key = kv.first;
        if (kv.second.is_object()) {
            if (auto* t = kv.second.find("type")) if (t->is_string())
                e.type = t->as_string();
            if (auto* d = kv.second.find("description")) if (d->is_string())
                e.description = d->as_string();
            if (auto* dv = kv.second.find("default")) {
                // 序列化 default 到 JSON 字符串
                if (dv->is_string()) e.default_json = "\"" + dv->as_string() + "\"";
                else if (dv->is_bool()) e.default_json = dv->as_bool() ? "true" : "false";
                else if (dv->is_int()) e.default_json = std::to_string(dv->as_int());
                else if (dv->is_null()) e.default_json = "null";
                else e.default_json = "null";
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

// 把 json_value 重新序列化成 utf-8 JSON string (保留原样, 供 mcpServers /
// chatProviders / sao_menu / locales 这些 opaque 字段用)。
std::string serialize_value(const json_value& v);

std::string serialize_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
    return out;
}

std::string serialize_value(const json_value& v) {
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_int()) return std::to_string(v.as_int());
    if (std::holds_alternative<double>(v.v)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%g", std::get<double>(v.v));
        return buf;
    }
    if (v.is_string()) return serialize_string(v.as_string());
    if (v.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& e : v.as_array()) {
            if (!first) out += ",";
            out += serialize_value(e);
            first = false;
        }
        out += "]";
        return out;
    }
    if (v.is_object()) {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : v.as_object()) {
            if (!first) out += ",";
            out += serialize_string(kv.first);
            out += ":";
            out += serialize_value(kv.second);
            first = false;
        }
        out += "}";
        return out;
    }
    return "null";
}

char* dup_c_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

int32_t populate_manifest_from_json(const json_value& root,
                                    sao::plugins::loader::plugin_manifest& out) {
    using namespace sao::plugins::loader;
    if (!root.is_object()) return SAO_ERR_INVALID_ARGUMENT;

    // ── 核心字段 ──
    if (auto* v = root.find("id")) if (v->is_string()) out.plugin_id = v->as_string();
    if (auto* v = root.find("name")) if (v->is_string()) out.name = v->as_string();
    if (auto* v = root.find("version")) if (v->is_string()) out.version = v->as_string();
    if (auto* v = root.find("description")) if (v->is_string()) out.description = v->as_string();
    if (auto* v = root.find("entry")) if (v->is_string()) out.entry = v->as_string();
    if (auto* v = root.find("enabled")) out.enabled = v->as_bool(false);

    // 缺失字段默认值 (对齐 Python `str(m.get(...) or default)` 语义)
    // 只在此处做, 不放 normalize_v1_manifest, 保证 parse 即返回规范化结构
    if (out.name.empty()) out.name = out.plugin_id;
    if (out.version.empty()) out.version = "0.1.0";

    // language 优先, 老 engine / runtime 别名 fallback
    std::string lang_str;
    if (auto* v = root.find("language")) if (v->is_string()) lang_str = v->as_string();
    if (lang_str.empty()) if (auto* v = root.find("engine")) if (v->is_string()) lang_str = v->as_string();
    if (lang_str.empty()) if (auto* v = root.find("runtime")) if (v->is_string()) lang_str = v->as_string();
    if (!lang_str.empty()) out.language = parse_language(lang_str);
    if (out.language == engine_kind::unknown && !out.entry.empty()) {
        out.language = infer_from_entry(out.entry);
    }
    if (out.language == engine_kind::unknown) {
        // 缺 language 且缺 entry → 默认 python (对齐 Python 平台老策略)
        out.language = engine_kind::python;
    }
    if (out.entry.empty()) out.entry = default_entry_for(out.language);

    // ── 依赖 / 权限 / 能力 ──
    if (auto* v = root.find("requires")) {
        out.requires_list = normalize_requires(*v);
    }
    if (auto* v = root.find("deps")) {
        // 老别名: deps → 加入 requires
        auto extra = normalize_requires(*v);
        for (auto& e : extra) out.requires_list.push_back(std::move(e));
    }
    if (auto* v = root.find("permissions")) if (v->is_array()) {
        out.permissions = array_to_string_list(v->as_array());
    }
    if (auto* v = root.find("game_ids")) if (v->is_array()) {
        out.game_ids = array_to_string_list(v->as_array());
    }
    if (auto* v = root.find("capabilities")) if (v->is_array()) {
        out.capabilities = normalize_capabilities(v->as_array());
    }

    // ── UI / 菜单 / 快捷键 ──
    if (auto* v = root.find("hotkeys")) out.hotkeys = normalize_hotkeys(*v);
    if (auto* v = root.find("sao_menu")) if (v->is_object()) {
        out.sao_menu_json = serialize_value(*v);
    }
    if (auto* v = root.find("settings_schema")) if (v->is_object()) {
        out.settings_schema = normalize_settings_schema(v->as_object());
    }
    // locales / i18n / translations 三选一 (任一命中即用)
    if (auto* v = root.find("locales")) if (v->is_object()) {
        out.locales_json = serialize_value(*v);
    }
    if (out.locales_json.empty()) if (auto* v = root.find("i18n")) if (v->is_object()) {
        out.locales_json = serialize_value(*v);
    }
    if (out.locales_json.empty()) if (auto* v = root.find("translations")) if (v->is_object()) {
        out.locales_json = serialize_value(*v);
    }

    // ── 面板标志 ──
    if (auto* v = root.find("primary")) out.primary = v->as_bool(true);
    if (auto* v = root.find("hidden")) out.hidden = v->as_bool(false);
    if (auto* v = root.find("min_width")) out.min_width = static_cast<uint32_t>(v->as_int(0));
    if (auto* v = root.find("min_height")) out.min_height = static_cast<uint32_t>(v->as_int(0));

    // ── AI Editor 桥接 ──
    if (auto* v = root.find("mcpServers")) out.mcp_servers_json = serialize_value(*v);
    if (auto* v = root.find("chatProviders")) out.chat_providers_json = serialize_value(*v);

    // ── Workshop 保护 ──
    if (auto* v = root.find("protected")) out.protected_plugin = v->as_bool(false);
    if (auto* v = root.find("native_entry")) if (v->is_string())
        out.native_entry = v->as_string();
    if (auto* v = root.find("native_abi")) if (v->is_string())
        out.native_abi = v->as_string();

    // ── ABI 版本 ──
    if (auto* v = root.find("abi_version")) out.abi_version = static_cast<uint32_t>(v->as_int(0));

    return SAO_OK;
}

} // namespace

// ── 公开 API 实装 ────────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_parse_manifest_json(const char* utf8_json_ptr,
                                       size_t utf8_json_len,
                                       sao::plugins::loader::plugin_manifest* out_manifest,
                                       char** out_error_utf8) {
    if (out_error_utf8 != nullptr) *out_error_utf8 = nullptr;
    if (out_manifest == nullptr || utf8_json_ptr == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;

    *out_manifest = sao::plugins::loader::plugin_manifest{};

    // 跳过 UTF-8 BOM
    std::string_view src(utf8_json_ptr, utf8_json_len);
    if (src.size() >= 3 &&
        static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        src.remove_prefix(3);
    }

    json_value root;
    std::string err;
    json_parser parser(src);
    if (!parser.parse(root, err)) {
        out_manifest->parse_error = err;
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(err);
        return SAO_ERR_INVALID_ARGUMENT;
    }

    int32_t rc = populate_manifest_from_json(root, *out_manifest);
    if (rc != SAO_OK) {
        std::string e = "manifest root is not a JSON object";
        out_manifest->parse_error = e;
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
    }
    return rc;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_load_manifest_file(const wchar_t* manifest_path,
                                      sao::plugins::loader::plugin_manifest* out_manifest,
                                      char** out_error_utf8) {
    if (out_error_utf8 != nullptr) *out_error_utf8 = nullptr;
    if (manifest_path == nullptr || out_manifest == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;

    std::ifstream fp(manifest_path, std::ios::binary);
    if (!fp.is_open()) {
        std::string e = "cannot open plugin.json";
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
        out_manifest->parse_error = e;
        return SAO_ERR_HANDLE_INVALID;
    }
    std::stringstream ss;
    ss << fp.rdbuf();
    std::string content = ss.str();
    return sao_plugins_compat_parse_manifest_json(content.data(), content.size(),
                                                   out_manifest, out_error_utf8);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_normalize_v1_manifest(sao::plugins::loader::plugin_manifest* manifest,
                                         char** out_warnings_json_utf8) {
    if (manifest == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (out_warnings_json_utf8 != nullptr) *out_warnings_json_utf8 = nullptr;

    std::vector<std::string> warnings;

    // 1. language 缺失 → 从 entry 猜, 缺失都用 python
    if (manifest->language == sao::plugins::loader::engine_kind::unknown) {
        if (!manifest->entry.empty()) {
            manifest->language = infer_from_entry(manifest->entry);
        }
        if (manifest->language == sao::plugins::loader::engine_kind::unknown) {
            manifest->language = sao::plugins::loader::engine_kind::python;
            warnings.push_back("language absent, defaulted to python");
        }
    }
    // 2. entry 缺失 → 猜
    if (manifest->entry.empty()) {
        manifest->entry = default_entry_for(manifest->language);
        warnings.push_back("entry absent, defaulted to " + manifest->entry);
    }
    // 3. abi_version == 0 → v1
    if (manifest->abi_version == 0) {
        manifest->abi_version = 1;
        warnings.push_back("abi_version absent, treated as v1 (should be 2)");
    }
    // 4. deprecated: name 缺失
    if (manifest->name.empty()) {
        manifest->name = manifest->plugin_id;
    }

    if (out_warnings_json_utf8 != nullptr) {
        std::string out = "[";
        bool first = true;
        for (auto& w : warnings) {
            if (!first) out += ",";
            out += serialize_string(w);
            first = false;
        }
        out += "]";
        *out_warnings_json_utf8 = dup_c_string(out);
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_normalize_requires(const char* requires_json_utf8,
                                      char** out_normalized_json_utf8) {
    if (requires_json_utf8 == nullptr || out_normalized_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_normalized_json_utf8 = nullptr;

    json_value root;
    std::string err;
    std::string_view src(requires_json_utf8);
    json_parser parser(src);
    if (!parser.parse(root, err)) return SAO_ERR_INVALID_ARGUMENT;

    auto list = normalize_requires(root);
    std::string out = "[";
    bool first = true;
    for (auto& e : list) {
        if (!first) out += ",";
        out += serialize_string(e);
        first = false;
    }
    out += "]";
    *out_normalized_json_utf8 = dup_c_string(out);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_compat_guess_entry(sao::plugins::loader::engine_kind language,
                               char** out_entry) {
    if (out_entry == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    const char* guess = default_entry_for(language);
    *out_entry = dup_c_string(guess);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_compat_free_string(char* str) {
    if (str != nullptr) std::free(str);
}

} // namespace sao::plugins::compat
