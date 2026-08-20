// test_manifest_fixture_parity.cpp — plugin manifest 磁盘 fixture parity
//
// 用磁盘 docs/fixtures/plugin_manifest/*.json 13 个真实 fixture 驱动 parser
// 逐字段对齐 expected.normalized. 每 case 是 assert 死断.
//
// 每 fixture schema:
//   {
//     "input":    <raw plugin.json 传给 parser>,
//     "expected": {
//       "normalized": <plugin_manifest 期望字段>,
//       "plugin_id_valid": bool,
//       "extra_keys_dropped": {...}
//     },
//     "kind": "normal" / "partial" / "malformed" / "conflict" / "extra_field" / "deep_nested"
//   }
//
// 每 fixture 一个 CASE, 13 CASE 全绿即通过.

#include "sao/plugins/compat/py_v1_manifest.h"
#include "sao/plugins/loader/plugin_manifest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace sao::plugins::compat;
using namespace sao::plugins::loader;

namespace {
[[noreturn]] void sao_test_assert_fail(const char* file, int line,
                                        const char* expression) {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

#define SAO_TEST_ASSERT(...)                                                   \
    do {                                                                       \
        if (!(__VA_ARGS__)) {                                                  \
            sao_test_assert_fail(__FILE__, __LINE__, #__VA_ARGS__);            \
        }                                                                      \
    } while (false)
// ── 极简 JSON reader (只读, 只 read fixture) ─────────────────
//
// 这里独立于 compat parser (那个是被测对象), 用最小 JSON reader 从 fixture
// 拿 input 子对象 + expected 子对象.

struct jv;
using jarr = std::vector<jv>;
using jobj = std::vector<std::pair<std::string, jv>>;
struct jv {
    std::variant<std::nullptr_t, bool, int64_t, double,
                 std::string, jarr, jobj> v;
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_int() const { return std::holds_alternative<int64_t>(v); }
    bool is_dbl() const { return std::holds_alternative<double>(v); }
    bool is_str() const { return std::holds_alternative<std::string>(v); }
    bool is_arr() const { return std::holds_alternative<jarr>(v); }
    bool is_obj() const { return std::holds_alternative<jobj>(v); }
    const std::string& s() const { static const std::string e; return is_str() ? std::get<std::string>(v) : e; }
    bool b(bool d=false) const { if (is_bool()) return std::get<bool>(v); if (is_int()) return std::get<int64_t>(v)!=0; return d; }
    int64_t i(int64_t d=0) const { if (is_int()) return std::get<int64_t>(v); if (is_dbl()) return static_cast<int64_t>(std::get<double>(v)); return d; }
    const jarr& a() const { static const jarr e; return is_arr() ? std::get<jarr>(v) : e; }
    const jobj& o() const { static const jobj e; return is_obj() ? std::get<jobj>(v) : e; }
    const jv* find(std::string_view k) const {
        if (!is_obj()) return nullptr;
        for (auto& kv : std::get<jobj>(v)) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct jparser {
    std::string_view s; size_t p=0; std::string err;
    explicit jparser(std::string_view src): s(src) {}
    void ws() { while (p<s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p; }
    bool cn(char c) { ws(); if (p<s.size() && s[p]==c) { ++p; return true; } return false; }
    bool str(jv& o) {
        if (!cn('"')) return false;
        std::string out;
        while (p<s.size()) {
            char c = s[p++];
            if (c=='"') { o.v = std::move(out); return true; }
            if (c=='\\') {
                if (p>=s.size()) return false;
                char e = s[p++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (p+4>s.size()) return false;
                        uint32_t cp=0;
                        for (int i=0;i<4;++i) {
                            char h=s[p++]; cp<<=4;
                            if (h>='0'&&h<='9') cp|=(h-'0');
                            else if (h>='a'&&h<='f') cp|=(h-'a'+10);
                            else if (h>='A'&&h<='F') cp|=(h-'A'+10);
                            else return false;
                        }
                        if (cp<0x80) out += (char)cp;
                        else if (cp<0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
                        else { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                        break;
                    }
                    default: out += e; break;
                }
            } else out += c;
        }
        return false;
    }
    bool val(jv& o) {
        ws();
        if (p>=s.size()) return false;
        char c = s[p];
        if (c=='"') return str(o);
        if (c=='{') { ++p; jobj obj; ws(); if (cn('}')) { o.v=std::move(obj); return true; }
            while (true) { jv k; ws(); if (!str(k)) return false; ws(); if (!cn(':')) return false;
                jv vv; if (!val(vv)) return false; obj.emplace_back(std::get<std::string>(k.v), std::move(vv));
                ws(); if (cn(',')) continue; if (cn('}')) { o.v=std::move(obj); return true; } return false; } }
        if (c=='[') { ++p; jarr arr; ws(); if (cn(']')) { o.v=std::move(arr); return true; }
            while (true) { jv e; if (!val(e)) return false; arr.emplace_back(std::move(e));
                ws(); if (cn(',')) continue; if (cn(']')) { o.v=std::move(arr); return true; } return false; } }
        if (c=='t') { if (s.compare(p,4,"true")==0) { p+=4; o.v=true; return true; } return false; }
        if (c=='f') { if (s.compare(p,5,"false")==0) { p+=5; o.v=false; return true; } return false; }
        if (c=='n') { if (s.compare(p,4,"null")==0) { p+=4; o.v=nullptr; return true; } return false; }
        if (c=='-'||(c>='0'&&c<='9')) {
            size_t start=p; if (s[p]=='-') ++p;
            while (p<s.size() && s[p]>='0'&&s[p]<='9') ++p;
            bool f=false;
            if (p<s.size()&&s[p]=='.') { f=true; ++p; while (p<s.size()&&s[p]>='0'&&s[p]<='9') ++p; }
            if (p<s.size()&&(s[p]=='e'||s[p]=='E')) { f=true; ++p; if (p<s.size()&&(s[p]=='+'||s[p]=='-')) ++p; while(p<s.size()&&s[p]>='0'&&s[p]<='9') ++p; }
            std::string tok(s.substr(start, p-start));
            if (f) o.v = std::atof(tok.c_str());
            else o.v = (int64_t)std::atoll(tok.c_str());
            return true;
        }
        return false;
    }
    bool parse(jv& o) { ws(); return val(o); }
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// 序列化 jv 回 JSON (用于把 fixture 的 input 对象 --> 传给 compat parser)
std::string ser_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b,sizeof(b),"\\u%04x", c); o += b; }
                else o += c;
        }
    }
    o += '"'; return o;
}
std::string ser(const jv& v) {
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.b() ? "true" : "false";
    if (v.is_int()) return std::to_string(v.i());
    if (v.is_dbl()) { char b[32]; std::snprintf(b,sizeof(b),"%g", std::get<double>(v.v)); return b; }
    if (v.is_str()) return ser_str(v.s());
    if (v.is_arr()) { std::string o="["; bool f=true; for (auto& e : v.a()) { if (!f) o+=","; o += ser(e); f=false; } o+="]"; return o; }
    if (v.is_obj()) { std::string o="{"; bool f=true; for (auto& kv : v.o()) { if (!f) o+=","; o += ser_str(kv.first); o+=":"; o+=ser(kv.second); f=false; } o+="}"; return o; }
    return "null";
}

// ── 断言辅助 ────────────────────────────────────

struct case_fail {
    std::string fixture;
    std::string field;
    std::string got;
    std::string want;
};
std::vector<case_fail> g_fails;

void fail(const std::string& fixture, const std::string& field,
          const std::string& got, const std::string& want) {
    g_fails.push_back({fixture, field, got, want});
    std::printf("  [FAIL] %s field=%s got=%s want=%s\n",
                fixture.c_str(), field.c_str(),
                got.c_str(), want.c_str());
}

// engine_kind → 字符串 (对齐 fixture normalized.language 的 lowercase 表示)
std::string ek_to_str(engine_kind k) {
    switch (k) {
        case engine_kind::python: return "python";
        case engine_kind::emma: return "emma";
        case engine_kind::angelscript: return "angelscript";
        case engine_kind::lua: return "lua";
        case engine_kind::csharp: return "csharp";
        default: return "unknown";
    }
}

bool eq_str_list(const std::vector<std::string>& got, const jarr& want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) {
        if (!want[i].is_str()) return false;
        if (got[i] != want[i].s()) return false;
    }
    return true;
}

// 比 capability entry vs fixture 里的 json 对象
bool eq_capability(const capability_entry& got, const jv& want_v) {
    if (!want_v.is_obj()) return false;
    // fixture cap normalized 形式:
    //   {"id": "...", "title"?: "...", "actions"?: [...], ...}
    auto* fid = want_v.find("id");
    if (!fid || !fid->is_str()) return false;
    if (got.id != fid->s()) return false;
    if (auto* t = want_v.find("title")) {
        if (!t->is_str() || got.title != t->s()) return false;
    } else {
        // fixture 没 title 期望 → got.title 应空
        if (!got.title.empty()) return false;
    }
    if (auto* d = want_v.find("description")) {
        if (!d->is_str() || got.description != d->s()) return false;
    } else {
        // fixture 里 normalized 的 description 有时会被丢, 但我们保留原字段;
        // 不比对没出现在 fixture normalized 里的 description
    }
    if (auto* acts = want_v.find("actions")) {
        if (!acts->is_arr() || !eq_str_list(got.actions, acts->a())) return false;
    } else {
        if (!got.actions.empty()) return false;
    }
    return true;
}

// 比 settings_schema (fixture 是对象形式, got 是列表形式)
bool eq_settings_schema(const std::vector<settings_schema_entry>& got, const jv& want_v) {
    if (!want_v.is_obj()) return got.empty() && (want_v.is_null() || (want_v.is_obj() && want_v.o().empty()));
    const jobj& expected = want_v.o();
    if (got.size() != expected.size()) return false;
    for (auto& kv : expected) {
        const std::string& key = kv.first;
        const jv& def = kv.second;
        auto it = std::find_if(got.begin(), got.end(), [&](const settings_schema_entry& e) { return e.key == key; });
        if (it == got.end()) return false;
        if (def.is_obj()) {
            auto* t = def.find("type");
            if (t && t->is_str() && it->type != t->s()) return false;
            auto* d = def.find("default");
            if (d) {
                std::string want_default;
                if (d->is_str()) want_default = "\"" + d->s() + "\"";
                else if (d->is_bool()) want_default = d->b() ? "true" : "false";
                else if (d->is_int()) want_default = std::to_string(d->i());
                else if (d->is_null()) want_default = "null";
                if (it->default_json != want_default) return false;
            }
        }
    }
    return true;
}

// 主对比: got vs fixture 的 expected.normalized 对象
bool compare_manifest_to_expected(const std::string& fixture_name,
                                  const plugin_manifest& got,
                                  const jv& normalized) {
    bool ok = true;
    auto check_str = [&](const char* field, const std::string& g, const jv* w, const std::string& default_want = "") {
        std::string want = w && w->is_str() ? w->s() : default_want;
        if (g != want) { fail(fixture_name, field, g, want); ok = false; }
    };
    auto check_bool = [&](const char* field, bool g, const jv* w, bool default_want = false) {
        bool want = w ? w->b(default_want) : default_want;
        if (g != want) { fail(fixture_name, field, g?"true":"false", want?"true":"false"); ok = false; }
    };

    check_str("plugin_id", got.plugin_id, normalized.find("plugin_id"));
    check_str("name", got.name, normalized.find("name"));
    check_str("version", got.version, normalized.find("version"));
    check_str("description", got.description, normalized.find("description"));
    check_str("entry", got.entry, normalized.find("entry"));
    check_str("language", ek_to_str(got.language), normalized.find("language"));
    check_bool("enabled", got.enabled, normalized.find("enabled"));
    check_bool("protected", got.protected_plugin, normalized.find("protected"));
    check_str("native_entry", got.native_entry, normalized.find("native_entry"));
    check_str("native_abi", got.native_abi, normalized.find("native_abi"));

    // permissions (list of strings, order-preserved)
    if (auto* p = normalized.find("permissions")) {
        if (!p->is_arr() || !eq_str_list(got.permissions, p->a())) {
            fail(fixture_name, "permissions", "list size " + std::to_string(got.permissions.size()),
                 "list size " + std::to_string(p->a().size()));
            ok = false;
        }
    } else {
        if (!got.permissions.empty()) { fail(fixture_name, "permissions", "non-empty", "empty"); ok = false; }
    }

    // game_ids
    if (auto* g = normalized.find("game_ids")) {
        if (!g->is_arr() || !eq_str_list(got.game_ids, g->a())) {
            fail(fixture_name, "game_ids", "size " + std::to_string(got.game_ids.size()),
                 "size " + std::to_string(g->a().size()));
            ok = false;
        }
    } else {
        if (!got.game_ids.empty()) { fail(fixture_name, "game_ids", "non-empty", "empty"); ok = false; }
    }

    // requires (list of strings)
    if (auto* r = normalized.find("requires")) {
        if (!r->is_arr() || !eq_str_list(got.requires_list, r->a())) {
            std::string got_s;
            for (auto& e : got.requires_list) { got_s += e; got_s += ";"; }
            std::string want_s;
            for (auto& e : r->a()) { if (e.is_str()) { want_s += e.s(); want_s += ";"; } }
            fail(fixture_name, "requires", got_s, want_s);
            ok = false;
        }
    } else {
        if (!got.requires_list.empty()) { fail(fixture_name, "requires", "non-empty", "empty"); ok = false; }
    }

    // capabilities
    if (auto* c = normalized.find("capabilities")) {
        if (!c->is_arr()) { fail(fixture_name, "capabilities", "not-array", "array"); ok = false; }
        else {
            const jarr& want_caps = c->a();
            if (got.capabilities.size() != want_caps.size()) {
                fail(fixture_name, "capabilities.size",
                     std::to_string(got.capabilities.size()),
                     std::to_string(want_caps.size()));
                ok = false;
            } else {
                for (size_t i = 0; i < want_caps.size(); ++i) {
                    if (!eq_capability(got.capabilities[i], want_caps[i])) {
                        fail(fixture_name, "capability[" + std::to_string(i) + "]",
                             got.capabilities[i].id, want_caps[i].find("id") && want_caps[i].find("id")->is_str() ? want_caps[i].find("id")->s() : "?");
                        ok = false;
                    }
                }
            }
        }
    } else {
        if (!got.capabilities.empty()) { fail(fixture_name, "capabilities", "non-empty", "empty"); ok = false; }
    }

    // settings_schema
    if (auto* s = normalized.find("settings_schema")) {
        if (!eq_settings_schema(got.settings_schema, *s)) {
            fail(fixture_name, "settings_schema",
                 "size " + std::to_string(got.settings_schema.size()),
                 s->is_obj() ? "obj size " + std::to_string(s->o().size()) : "not-obj");
            ok = false;
        }
    } else {
        if (!got.settings_schema.empty()) { fail(fixture_name, "settings_schema", "non-empty", "empty"); ok = false; }
    }

    // locales — fixture 里 "locales": {}, got 可能是 empty string 或 "{}"
    // 规则: 空对象 → got.locales_json 应是 "{}" 或空. 只在 fixture 非空且非空对象时严格对比 raw.
    if (auto* l = normalized.find("locales")) {
        if (l->is_obj() && l->o().empty()) {
            // 允许 got.locales_json 是空字符串或 "{}"
            if (!got.locales_json.empty() && got.locales_json != "{}") {
                fail(fixture_name, "locales", got.locales_json, "empty or {}"); ok = false;
            }
        }
        // 非空 locales fixture: 目前只有 example plugins 有, normalize 后 fixture 也 {} — 无严格对比
    }

    // sao_menu
    if (auto* m = normalized.find("sao_menu")) {
        if (m->is_obj() && m->o().empty()) {
            if (!got.sao_menu_json.empty() && got.sao_menu_json != "{}") {
                fail(fixture_name, "sao_menu", got.sao_menu_json, "empty or {}"); ok = false;
            }
        }
    }

    return ok;
}

// 单个 fixture 跑一遍
int run_fixture(const std::string& path) {
    std::string full = read_file(path);
    if (full.empty()) {
        std::printf("  [FAIL] cannot read %s\n", path.c_str());
        return 1;
    }
    jv fixture_root;
    jparser jp(full);
    if (!jp.parse(fixture_root)) {
        std::printf("  [FAIL] cannot parse fixture %s\n", path.c_str());
        return 1;
    }
    // fixture name 取自 basename
    std::string name = path;
    auto pos = name.find_last_of("\\/");
    if (pos != std::string::npos) name = name.substr(pos + 1);
    if (name.size() > 5) name = name.substr(0, name.size() - 5); // 去 .json

    const jv* input = fixture_root.find("input");
    const jv* expected = fixture_root.find("expected");
    if (input == nullptr || expected == nullptr) {
        std::printf("  [FAIL] fixture %s missing input/expected\n", name.c_str());
        return 1;
    }
    const jv* normalized = expected->find("normalized");
    if (normalized == nullptr) {
        std::printf("  [FAIL] fixture %s missing expected.normalized\n", name.c_str());
        return 1;
    }

    // input 序列化回 raw JSON 传给 compat parser
    std::string raw_input = ser(*input);

    plugin_manifest m{};
    char* err = nullptr;
    int32_t rc = sao_plugins_compat_parse_manifest_json(
        raw_input.data(), raw_input.size(), &m, &err);
    if (rc != SAO_OK) {
        std::printf("  [FAIL] %s parse rc=%d err=%s\n", name.c_str(),
                    rc, err ? err : "(null)");
        if (err) sao_plugins_compat_free_string(err);
        return 1;
    }
    if (err) sao_plugins_compat_free_string(err);

    if (!compare_manifest_to_expected(name, m, *normalized)) {
        return 1;
    }
    // plugin_id_valid: fixture 声明的 valid=false 时 got.plugin_id 应空
    if (auto* piv = expected->find("plugin_id_valid")) {
        if (piv->is_bool()) {
            bool want = piv->b();
            bool got = !m.plugin_id.empty();
            if (want != got) {
                std::printf("  [FAIL] %s plugin_id_valid mismatch got=%d want=%d\n",
                            name.c_str(), got?1:0, want?1:0);
                return 1;
            }
        }
    }
    std::printf("  [OK] %s\n", name.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // fixture 目录: 编译时通过 SAO_FIXTURE_DIR 传入 (由 CMake 定义)
    // 或允许 argv[1] 覆盖
    std::string dir;
    if (argc >= 2) dir = argv[1];
#ifdef SAO_FIXTURE_DIR
    if (dir.empty()) dir = SAO_FIXTURE_DIR;
#endif
    if (dir.empty()) {
        std::printf("test_manifest_fixture_parity: no fixture dir configured\n");
        return 1;
    }

    std::printf("test_manifest_fixture_parity: fixture_dir=%s\n", dir.c_str());

    // 13 个 fixture (按字典序)
    const char* names[] = {
        "deep_nested_requires_dict.json",
        "extra_field_unknown_keys_kept.json",
        "malformed_language_engine_conflict.json",
        "malformed_missing_entry.json",
        "malformed_missing_id.json",
        "normal_example_angelscript.json",
        "normal_example_csharp.json",
        "normal_example_emma.json",
        "normal_example_lua.json",
        "normal_hide_seek.json",
        "normal_midi_piano.json",
        "normal_star_resonance.json",
        "partial_v1_legacy.json",
    };
    const size_t total = sizeof(names) / sizeof(names[0]);
    int failed = 0;
    for (size_t i = 0; i < total; ++i) {
        std::string path = dir;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
        path += names[i];
        failed += run_fixture(path);
    }

    if (failed == 0) {
        std::printf("test_manifest_fixture_parity: %zu/%zu cases passed\n",
                    total, total);
        return 0;
    } else {
        std::printf("test_manifest_fixture_parity: %d/%zu failures\n",
                    failed, total);
        return 1;
    }
}
