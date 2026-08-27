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
#include <charconv>
#include <cerrno>
#include <cmath>
#include <limits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sao::plugins::compat {

namespace {

// ── 微型 JSON 解析器 ────────────────────────────────

constexpr size_t kMaximumManifestFileBytes = 1024U * 1024U;
constexpr size_t kMaximumManifestRawBytes = 1024U * 1024U;
constexpr size_t kMaximumManifestJsonDepth = 64U;
constexpr size_t kMaximumManifestJsonNodes = 16384U;
constexpr size_t kMaximumManifestStringBytes = 64U * 1024U;
constexpr size_t kMaximumManifestTotalStringBytes = 512U * 1024U;
constexpr size_t kMaximumRequiresRawBytes = 256U * 1024U;
constexpr size_t kMaximumRequiresOutputBytes = 256U * 1024U;

bool valid_manifest_utf8(std::string_view value) {
    size_t index = 0;
    while (index < value.size()) {
        const auto byte = static_cast<unsigned char>(value[index]);
        size_t width = 0;
        uint32_t code_point = 0;
        if (byte <= 0x7fU) { width = 1; code_point = byte; }
        else if (byte >= 0xc2U && byte <= 0xdfU) { width = 2; code_point = byte & 0x1fU; }
        else if (byte >= 0xe0U && byte <= 0xefU) { width = 3; code_point = byte & 0x0fU; }
        else if (byte >= 0xf0U && byte <= 0xf4U) { width = 4; code_point = byte & 0x07U; }
        else return false;
        if (index + width > value.size()) return false;
        for (size_t offset = 1; offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((width == 2U && code_point < 0x80U) ||
            (width == 3U && code_point < 0x800U) ||
            (width == 4U && code_point < 0x10000U) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) return false;
        index += width;
    }
    return true;
}

struct json_value;
using json_object = std::vector<std::pair<std::string, json_value>>;
using json_array = std::vector<json_value>;

struct json_value {
    // 顺序: null, bool, signed int, unsigned int, double, string, array, object
    std::variant<std::nullptr_t, bool, int64_t, uint64_t, double,
                 std::string, json_array, json_object> v;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_int() const { return std::holds_alternative<int64_t>(v); }
    bool is_unsigned() const { return std::holds_alternative<uint64_t>(v); }
    bool is_number() const {
        return is_int() || is_unsigned() || std::holds_alternative<double>(v);
    }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_array() const { return std::holds_alternative<json_array>(v); }
    bool is_object() const { return std::holds_alternative<json_object>(v); }

    bool as_bool(bool fallback = false) const {
        if (is_bool()) return std::get<bool>(v);
        if (is_int()) return std::get<int64_t>(v) != 0;
        if (is_unsigned()) return std::get<uint64_t>(v) != 0;
        return fallback;
    }
    int64_t as_int(int64_t fallback = 0) const {
        if (is_int()) return std::get<int64_t>(v);
        if (is_unsigned()) {
            const auto value = std::get<uint64_t>(v);
            return value <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())
                       ? static_cast<int64_t>(value)
                       : fallback;
        }
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
        if (pos_ != src_.size()) {
            err = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    std::string_view src_;
    size_t pos_;
    std::string err_;
    size_t nodes_ = 0;
    size_t string_bytes_ = 0;

    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
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
        if (nodes_ >= kMaximumManifestJsonNodes) { err_ = "JSON node budget exceeded"; return false; }
        ++nodes_;
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
        std::string value;
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                if (value.size() > kMaximumManifestStringBytes ||
                    string_bytes_ > kMaximumManifestTotalStringBytes - value.size() ||
                    value.find('\0') != std::string::npos || !valid_manifest_utf8(value)) {
                    err_ = "invalid or oversized UTF-8 string";
                    return false;
                }
                string_bytes_ += value.size();
                out.v = std::move(value);
                return true;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) { err_ = "eof in escape"; return false; }
                const char escape = src_[pos_++];
                switch (escape) {
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    case '/': value += '/'; break;
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case 'r': value += '\r'; break;
                    case 'b': value += '\b'; break;
                    case 'f': value += '\f'; break;
                    case 'u': {
                        uint32_t code_point = 0;
                        if (!parse_hex_quad(code_point)) return false;
                        if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                            if (pos_ + 6 > src_.size() || src_[pos_] != '\\' ||
                                src_[pos_ + 1] != 'u') {
                                err_ = "lone high surrogate";
                                return false;
                            }
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parse_hex_quad(low) || low < 0xdc00U || low > 0xdfffU) {
                                err_ = "invalid surrogate pair";
                                return false;
                            }
                            code_point = 0x10000U + ((code_point - 0xd800U) << 10U) +
                                         (low - 0xdc00U);
                        } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
                            err_ = "lone low surrogate";
                            return false;
                        }
                        append_code_point(value, code_point);
                        break;
                    }
                    default: err_ = "unknown escape"; return false;
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20U) {
                    err_ = "unescaped control character";
                    return false;
                }
                value += c;
            }
            if (value.size() > kMaximumManifestStringBytes) {
                err_ = "string exceeds byte budget";
                return false;
            }
        }
        err_ = "unterminated string";
        return false;
    }

    bool parse_hex_quad(uint32_t& output) {
        if (pos_ + 4 > src_.size()) {
            err_ = "bad \\u escape";
            return false;
        }
        output = 0;
        for (int index = 0; index < 4; ++index) {
            const char value = src_[pos_++];
            output <<= 4U;
            if (value >= '0' && value <= '9') output |= static_cast<uint32_t>(value - '0');
            else if (value >= 'a' && value <= 'f') output |= static_cast<uint32_t>(value - 'a' + 10);
            else if (value >= 'A' && value <= 'F') output |= static_cast<uint32_t>(value - 'A' + 10);
            else {
                err_ = "bad hex in \\u";
                return false;
            }
        }
        return true;
    }

    void append_code_point(std::string& output, uint32_t code_point) {
        if (code_point < 0x80U) {
            output += static_cast<char>(code_point);
        } else if (code_point < 0x800U) {
            output += static_cast<char>(0xc0U | (code_point >> 6U));
            output += static_cast<char>(0x80U | (code_point & 0x3fU));
        } else if (code_point < 0x10000U) {
            output += static_cast<char>(0xe0U | (code_point >> 12U));
            output += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU));
            output += static_cast<char>(0x80U | (code_point & 0x3fU));
        } else {
            output += static_cast<char>(0xf0U | (code_point >> 18U));
            output += static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU));
            output += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU));
            output += static_cast<char>(0x80U | (code_point & 0x3fU));
        }
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
        const size_t start = pos_;
        const bool negative = src_[pos_] == '-';
        if (negative) ++pos_;
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            err_ = "bad number";
            return false;
        }
        if (src_[pos_] == '0' && pos_ + 1U < src_.size() &&
            std::isdigit(static_cast<unsigned char>(src_[pos_ + 1U]))) {
            err_ = "leading zero in number";
            return false;
        }
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        bool is_float = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_float = true;
            ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                err_ = "bad number fraction";
                return false;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                err_ = "bad number exponent";
                return false;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        const std::string token(src_.substr(start, pos_ - start));
        char* end = nullptr;
        errno = 0;
        if (is_float) {
            const double value = std::strtod(token.c_str(), &end);
            if (errno == ERANGE || end != token.c_str() + token.size() || !std::isfinite(value)) {
                err_ = "number out of range";
                return false;
            }
            out.v = value;
        } else if (negative) {
            const long long value = std::strtoll(token.c_str(), &end, 10);
            if (errno == ERANGE || end != token.c_str() + token.size()) {
                err_ = "integer out of range";
                return false;
            }
            out.v = static_cast<int64_t>(value);
        } else {
            const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
            if (errno == ERANGE || end != token.c_str() + token.size()) {
                err_ = "unsigned integer out of range";
                return false;
            }
            if (value > static_cast<unsigned long long>((std::numeric_limits<int64_t>::max)())) {
                err_ = "unsigned integer exceeds INT64_MAX";
                return false;
            }
            out.v = static_cast<int64_t>(value);
        }
        return true;
    }
};

// ── 字段规范化辅助 ──────────────────────────────────

std::string dup_utf8_string(const std::string& s) {
    return s;
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

bool valid_relative_entry(std::string_view value) {
    if (value.empty() || value.size() > 1024)
        return false;
    const auto path = std::filesystem::u8path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& part : path) {
        if (part == std::filesystem::path(".."))
            return false;
    }
    return true;
}



sao::plugins::loader::engine_kind parse_language(const std::string& raw) {
    std::string normalized = raw;
    const auto first = normalized.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return sao::plugins::loader::engine_kind::unknown;
    normalized = normalized.substr(first, normalized.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](char ch) {
        return ch == '-' || ch == '_';
    }), normalized.end());
    if (normalized == "python" || normalized == "py") return sao::plugins::loader::engine_kind::python;
    if (normalized == "emma") return sao::plugins::loader::engine_kind::emma;
    if (normalized == "angelscript" || normalized == "as" || normalized == "angel")
        return sao::plugins::loader::engine_kind::angelscript;
    if (normalized == "lua") return sao::plugins::loader::engine_kind::lua;
    if (normalized == "csharp" || normalized == "cs" || normalized == "c#" ||
        normalized == ".net" || normalized == "dotnet")
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
        case sao::plugins::loader::engine_kind::csharp: return "plugin.dll";
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
// json_parser 以对象出现顺序保存字段，单次扫描即可保持 Python dict 的顺序。


std::string python_repr_string(std::string_view value) {
    const char quote = value.find('\'') != std::string_view::npos and
                               value.find('"') == std::string_view::npos ? '"' : '\'';
    std::string out(1, quote);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\\') out += "\\\\";
        else if (character == quote) { out += "\\"; out.push_back(quote); }
        else if (character == '\n') out += "\\n";
        else if (character == '\r') out += "\\r";
        else if (character == '\t') out += "\\t";
        else if (byte < 0x20U or byte == 0x7fU) {
            char escaped[5]{};
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
            out += escaped;
        } else out.push_back(character);
    }
    out.push_back(quote);
    return out;
}

std::string python_string(const json_value& value, bool nested = false) {
    if (value.is_null()) return "None";
    if (value.is_bool()) return value.as_bool() ? "True" : "False";
    if (value.is_int()) return std::to_string(value.as_int());
    if (value.is_unsigned()) return std::to_string(std::get<uint64_t>(value.v));
    if (std::holds_alternative<double>(value.v)) {
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(value.v));
        if (result.ec != std::errc{}) return {};
        std::string output(buffer, result.ptr);
        if (output.find_first_of(".eE") == std::string::npos) output += ".0";
        return output;
    }
    if (value.is_string()) {
        if (!nested) return value.as_string();
        return python_repr_string(value.as_string());
    }
    if (value.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value.as_array()) {
            if (!first) out += ", ";
            out += python_string(item, true);
            first = false;
        }
        out += "]";
        return out;
    }
    if (value.is_object()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, item] : value.as_object()) {
            if (!first) out += ", ";
            out += python_string(json_value{key}, true) + ": " + python_string(item, true);
            first = false;
        }
        out += "}";
        return out;
    }
    return {};
}

bool python_truthy(const json_value& value) {
    if (value.is_null()) return false;
    if (value.is_bool()) return value.as_bool();
    if (value.is_int()) return value.as_int() != 0;
    if (value.is_unsigned()) return std::get<uint64_t>(value.v) != 0;
    if (std::holds_alternative<double>(value.v)) return std::get<double>(value.v) != 0.0;
    if (value.is_string()) return !value.as_string().empty();
    if (value.is_array()) return !value.as_array().empty();
    if (value.is_object()) return !value.as_object().empty();
    return false;
}

void append_requirement(std::vector<std::string>& out, std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return;
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (!value.empty()) out.push_back(std::move(value));
}

void append_python_requirement(std::vector<std::string>& out, const json_value& value) {
    if (python_truthy(value)) append_requirement(out, python_string(value));
}

std::vector<std::string> normalize_requires(const json_value& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const auto& item : value.as_array()) append_python_requirement(out, item);
    } else if (value.is_null() || value.is_bool() || value.is_number() || value.is_string()) {
        append_python_requirement(out, value);
    } else if (value.is_object()) {
        for (const auto& [raw_key, item] : value.as_object()) {
            const auto first = raw_key.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            const auto last = raw_key.find_last_not_of(" \t\r\n");
            const auto key = raw_key.substr(first, last - first + 1);
            if (key == "runtime_features" && item.is_array()) {
                for (const auto& feature : item.as_array())
                    if (python_truthy(feature))
                        append_requirement(out, "runtime_feature:" + python_string(feature));
            } else if (item.is_bool() && item.as_bool()) {
                append_requirement(out, key);
            } else if (python_truthy(item)) {
                append_requirement(out, key + python_string(item));
            }
        }
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

std::string serialize_value(const json_value& v);

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
            if (auto* dv = kv.second.find("default")) e.default_json = serialize_value(*dv);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// 把 json_value 重新序列化成 utf-8 JSON string (保留原样, 供 mcpServers /
// chatProviders / sao_menu / locales 这些 opaque 字段用）。

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
    if (v.is_unsigned()) return std::to_string(std::get<uint64_t>(v.v));
    if (std::holds_alternative<double>(v.v)) {
        char buf[64]{};
        const auto result = std::to_chars(buf, buf + sizeof(buf), std::get<double>(v.v));
        if (result.ec != std::errc{}) return {};
        std::string output(buf, result.ptr);
        if (output.find_first_of(".eE") == std::string::npos) output += ".0";
        return output;
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
    if (auto* v = root.find("managed_type")) if (v->is_string()) out.managed_type = v->as_string();
    if (auto* v = root.find("runtimeconfig")) if (v->is_string()) out.runtimeconfig = v->as_string();
    if (auto* v = root.find("native_entry")) if (v->is_string()) out.native_entry = v->as_string();
    if (auto* v = root.find("native_abi")) if (v->is_string()) out.native_abi = v->as_string();
    if (out.entry.empty() && !out.native_entry.empty()) out.entry = out.native_entry;
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
    if (utf8_json_len > kMaximumManifestRawBytes) return SAO_ERR_INVALID_ARGUMENT;

    // 跳过 UTF-8 BOM
    std::string_view src(utf8_json_ptr, utf8_json_len);
    if (!valid_manifest_utf8(src)) return SAO_ERR_INVALID_ARGUMENT;
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

    *out_manifest = sao::plugins::loader::plugin_manifest{};
    const std::filesystem::path requested_path(manifest_path);
    const auto requested_root = requested_path.has_parent_path()
                                    ? requested_path.parent_path()
                                    : std::filesystem::current_path();
    std::filesystem::path resolved_path;
    const int32_t containment_status =
        sao::plugins::loader::resolve_contained_existing_path(requested_root, requested_path,
                                                               resolved_path);
    if (containment_status != SAO_OK) {
        const std::string e = "plugin.json path is invalid or outside its root";
        out_manifest->parse_error = e;
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
        return containment_status;
    }

    std::ifstream fp(resolved_path, std::ios::binary);
    if (!fp.is_open()) {
        const std::string e = "cannot open plugin.json";
        out_manifest->parse_error = e;
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
        return SAO_ERR_HANDLE_INVALID;
    }
    fp.seekg(0, std::ios::end);
    const std::streampos end = fp.tellg();
    if (end < 0 or static_cast<uintmax_t>(end) > kMaximumManifestFileBytes) {
        const std::string e = "manifest exceeds raw byte budget";
        out_manifest->parse_error = e;
        if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
        return SAO_ERR_INVALID_ARGUMENT;
    }
    fp.seekg(0, std::ios::beg);
    std::string content(static_cast<size_t>(end), '\0');
    if (!content.empty()) {
        fp.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (fp.gcount() != static_cast<std::streamsize>(content.size()))
            return SAO_ERR_OS_CALL_FAILED;
    }
    if (fp.peek() != std::char_traits<char>::eof()) {
        out_manifest->parse_error = "manifest changed while being read";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_plugins_compat_parse_manifest_json(
        content.data(), content.size(), out_manifest, out_error_utf8);
    if (status != SAO_OK)
        return status;

    const auto plugin_root = resolved_path.parent_path();
    for (const auto& entry : {out_manifest->entry, out_manifest->native_entry,
                              out_manifest->runtimeconfig}) {
        if (entry.empty())
            continue;
        if (!valid_relative_entry(entry)) {
            const std::string e = "manifest entry path is invalid";
            out_manifest->parse_error = e;
            if (out_error_utf8 != nullptr) *out_error_utf8 = dup_c_string(e);
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto entry_path = plugin_root / std::filesystem::u8path(entry);
        std::error_code error;
        if (!std::filesystem::exists(entry_path, error)) {
            if (error)
                return SAO_ERR_OS_CALL_FAILED;
            continue;
        }
        std::filesystem::path resolved_entry;
        const int32_t entry_status = sao::plugins::loader::resolve_contained_existing_path(
            plugin_root, entry_path, resolved_entry);
        if (entry_status != SAO_OK)
            return entry_status;
    }
    out_manifest->source_path = path_utf8(plugin_root);
    if (out_manifest->source_path.empty())
        return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
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

    const auto* terminator = static_cast<const char*>(
        std::memchr(requires_json_utf8, '\0', kMaximumRequiresRawBytes + 1U));
    if (terminator == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    const size_t raw_size = static_cast<size_t>(terminator - requires_json_utf8);
    if (raw_size == 0 || raw_size > kMaximumRequiresRawBytes)
        return SAO_ERR_INVALID_ARGUMENT;

    json_value root;
    std::string err;
    std::string_view src(requires_json_utf8, raw_size);
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
    if (out.size() + 1U > kMaximumRequiresOutputBytes)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_normalized_json_utf8 = dup_c_string(out);
    return *out_normalized_json_utf8 == nullptr ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
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
