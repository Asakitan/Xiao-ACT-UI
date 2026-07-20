// SAO Auto — platform/core/src/config.cpp
//
// Settings envelope implementation.
//
// This translation unit provides two API families:
//
//   * ``sao_core_config_*`` — the older dotted-path facade, backed by the
//     settings envelope below.
//
//   * ``sao_core_settings_*`` — the dict-of-variant store backed by
//     a JSON file whose on-disk shape matches Python's
//     ``json.dumps(indent=2, sort_keys=True)`` byte-for-byte, so a config
//     written by C and one written by Python can be diffed cleanly.
//
// The facade delegates storage and persistence to the settings API so both
// families share one JSON implementation.

#include "sao/core/config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

using nlohmann::json;

struct sao_core_settings_s {
    using Value = std::variant<int64_t, double, bool, std::string>;
    std::unordered_map<std::string, Value> data;
};

struct sao_core_config_s {
    std::string backing_path;
    sao_core_settings_t* settings;
};

namespace {

bool config_contains_key(sao_core_config_handle_t handle, const char* key_utf8) {
    return handle->settings->data.find(key_utf8) != handle->settings->data.end();
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_open(
    const char* backing_path_utf8, sao_core_config_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (backing_path_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    sao_core_settings_t* settings = nullptr;
    const sao_status_t load_status =
        sao_core_settings_load(backing_path_utf8, &settings);
    if (load_status != SAO_STATUS_OK) return load_status;

    try {
        *out_handle = new sao_core_config_s{backing_path_utf8, settings};
    } catch (const std::bad_alloc&) {
        sao_core_settings_free(settings);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" void SAO_CORE_CALL sao_core_config_close(
    sao_core_config_handle_t handle) {
    if (handle == nullptr) return;
    sao_core_settings_free(handle->settings);
    delete handle;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_flush(
    sao_core_config_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao_core_settings_save(handle->backing_path.c_str(), handle->settings);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_get_bool(
    sao_core_config_handle_t handle, const char* key_utf8, bool* out_value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!config_contains_key(handle, key_utf8)) return SAO_STATUS_ERR_NOT_FOUND;
    return sao_core_settings_get_bool(handle->settings, key_utf8, false, out_value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_get_int(
    sao_core_config_handle_t handle, const char* key_utf8, int64_t* out_value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!config_contains_key(handle, key_utf8)) return SAO_STATUS_ERR_NOT_FOUND;
    return sao_core_settings_get_int(handle->settings, key_utf8, 0, out_value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_get_double(
    sao_core_config_handle_t handle, const char* key_utf8, double* out_value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!config_contains_key(handle, key_utf8)) return SAO_STATUS_ERR_NOT_FOUND;
    return sao_core_settings_get_float(handle->settings, key_utf8, 0.0, out_value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_get_string(
    sao_core_config_handle_t handle, const char* key_utf8,
    char* out_buffer, size_t buffer_len, size_t* out_bytes_needed) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!config_contains_key(handle, key_utf8)) return SAO_STATUS_ERR_NOT_FOUND;
    return sao_core_settings_get_string(handle->settings, key_utf8, nullptr,
                                        out_buffer, buffer_len,
                                        out_bytes_needed);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_set_bool(
    sao_core_config_handle_t handle, const char* key_utf8, bool value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao_core_settings_set_bool(handle->settings, key_utf8, value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_set_int(
    sao_core_config_handle_t handle, const char* key_utf8, int64_t value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao_core_settings_set_int(handle->settings, key_utf8, value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_set_double(
    sao_core_config_handle_t handle, const char* key_utf8, double value) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao_core_settings_set_float(handle->settings, key_utf8, value);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_set_string(
    sao_core_config_handle_t handle, const char* key_utf8,
    const char* value_utf8) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return sao_core_settings_set_string(handle->settings, key_utf8, value_utf8);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_config_erase(
    sao_core_config_handle_t handle, const char* key_utf8) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    handle->settings->data.erase(key_utf8);
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Flat settings envelope
// ---------------------------------------------------------------------------
namespace {

// Serialise the settings dict in the same shape Python emits with
// ``json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False)``.
// nlohmann::json's own ``dump(2)`` uses a slightly different layout
// (spaces inside arrays, ``:`` spacing) so we can't rely on it; instead
// we recurse over an intermediate ordered map + emit deterministic
// output ourselves.
//
// The base settings dict is flat (no nested arrays / objects), so
// the writer below deliberately only handles the flat case — deeper
// structures round-trip through nlohmann::json for the reader path but
// are rejected by the writer with SAO_STATUS_ERR_INVALID_ARGUMENT so we
// don't silently produce Python-incompatible output.
void appendEscapedJsonString(std::string& out, const std::string& in) {
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (c < 0x20) {
                    // Control character — emit as \u00xx.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

// Emit a JSON number.  Python's ``json`` module emits integers without a
// decimal point and floats with the shortest round-trip representation.
// We approximate that: ints go through %lld, floats through the standard
// nlohmann::json path which itself uses grisu2 shortest.
std::string formatValue(const sao_core_settings_s::Value& v) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int64_t>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(arg));
            return std::string(buf);
        } else if constexpr (std::is_same_v<T, double>) {
            // Route floats through nlohmann so we inherit its
            // shortest-round-trip float writer, then strip the trailing
            // ``.0`` mismatch by leaving it as-is — Python emits ``1.0``
            // for ``1.0`` too, so this stays aligned.
            json j = arg;
            return j.dump();
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? std::string("true") : std::string("false");
        } else {
            std::string out;
            appendEscapedJsonString(out, arg);
            return out;
        }
    }, v);
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_create(
    sao_core_settings_t** out_settings) {
    if (out_settings == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        *out_settings = new sao_core_settings_s{};
    } catch (const std::bad_alloc&) {
        *out_settings = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_load(
    const char* path_utf8, sao_core_settings_t** out_settings) {
    if (path_utf8 == nullptr || out_settings == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_settings = nullptr;

    // Missing file is not an error — hand back an empty envelope so the
    // launcher can seed defaults and save() on shutdown to bootstrap the
    // file on first launch.  This matches the Python SettingsManager
    // behaviour (no file → empty dict, no traceback).
    std::ifstream in(path_utf8, std::ios::binary);
    if (!in.good()) {
        return sao_core_settings_create(out_settings);
    }

    std::stringstream buf;
    buf << in.rdbuf();
    const std::string blob = buf.str();

    json parsed;
    try {
        parsed = json::parse(blob);
    } catch (const json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (!parsed.is_object()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    sao_core_settings_t* handle = nullptr;
    sao_status_t rc = sao_core_settings_create(&handle);
    if (rc != SAO_STATUS_OK) return rc;

    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        const std::string& key = it.key();
        const json& val = it.value();
        if (val.is_number_integer() || val.is_number_unsigned()) {
            handle->data[key] = static_cast<int64_t>(val.get<int64_t>());
        } else if (val.is_number_float()) {
            handle->data[key] = val.get<double>();
        } else if (val.is_boolean()) {
            handle->data[key] = val.get<bool>();
        } else if (val.is_string()) {
            handle->data[key] = val.get<std::string>();
        } else {
            // Nested arrays / objects / nulls aren't part of the flat settings
            // envelope contract — surface them as invalid so the caller
            // fails loudly rather than losing data on a subsequent save.
            sao_core_settings_free(handle);
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }

    *out_settings = handle;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_save(
    const char* path_utf8, const sao_core_settings_t* settings) {
    if (path_utf8 == nullptr || settings == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Sort keys deterministically.  Python's ``sort_keys=True`` uses
    // codepoint ordering which is the natural std::map / std::set order
    // for a UTF-8 byte comparison — pushing keys through std::map picks
    // that up for free.
    std::vector<std::string> keys;
    keys.reserve(settings->data.size());
    for (const auto& kv : settings->data) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    std::string out;
    if (keys.empty()) {
        // Python emits ``{}`` for an empty dict with indent=2.
        out = "{}";
    } else {
        out.reserve(64u * keys.size());
        out.append("{\n");
        for (size_t i = 0; i < keys.size(); ++i) {
            out.append("  ");
            appendEscapedJsonString(out, keys[i]);
            out.append(": ");
            auto it = settings->data.find(keys[i]);
            out.append(formatValue(it->second));
            if (i + 1 < keys.size()) out.push_back(',');
            out.push_back('\n');
        }
        out.push_back('}');
    }

    // Write atomically-ish: dump to a temp file, then rename.  On
    // Windows ``std::rename`` fails if the target exists; use the C
    // library ``rename`` after removing the target for simplicity.
    // This basic persistence path uses remove + rename rather than
    // ``ReplaceFileW``; callers needing stronger transactions use the
    // launcher-owned secure settings path.
    std::string tmp_path = std::string(path_utf8) + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f.good()) return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    std::remove(path_utf8);
    if (std::rename(tmp_path.c_str(), path_utf8) != 0) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

extern "C" void SAO_CORE_CALL sao_core_settings_free(
    sao_core_settings_t* settings) {
    delete settings;
}

// ---------------------------------------------------------------------------
// Getters — never fail on missing key; write the caller's default.
// ---------------------------------------------------------------------------
extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_get_int(
    const sao_core_settings_t* settings, const char* key_utf8,
    int64_t default_value, int64_t* out_value) {
    if (settings == nullptr || key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto it = settings->data.find(key_utf8);
    if (it == settings->data.end()) {
        *out_value = default_value;
        return SAO_STATUS_OK;
    }
    if (const auto* p = std::get_if<int64_t>(&it->second)) {
        *out_value = *p;
        return SAO_STATUS_OK;
    }
    // Silent coercion of double → int is a footgun — surface the type
    // mismatch instead so callers know their settings.json disagrees
    // with the code.
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_get_bool(
    const sao_core_settings_t* settings, const char* key_utf8,
    bool default_value, bool* out_value) {
    if (settings == nullptr || key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto it = settings->data.find(key_utf8);
    if (it == settings->data.end()) {
        *out_value = default_value;
        return SAO_STATUS_OK;
    }
    if (const auto* p = std::get_if<bool>(&it->second)) {
        *out_value = *p;
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_get_float(
    const sao_core_settings_t* settings, const char* key_utf8,
    double default_value, double* out_value) {
    if (settings == nullptr || key_utf8 == nullptr || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto it = settings->data.find(key_utf8);
    if (it == settings->data.end()) {
        *out_value = default_value;
        return SAO_STATUS_OK;
    }
    if (const auto* p = std::get_if<double>(&it->second)) {
        *out_value = *p;
        return SAO_STATUS_OK;
    }
    // Allow int → float widening: JSON authors often write ``1`` when
    // they meant ``1.0``, and losing precision here would just create
    // spurious type mismatches on every settings file that omitted
    // the trailing ``.0``.
    if (const auto* p = std::get_if<int64_t>(&it->second)) {
        *out_value = static_cast<double>(*p);
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_get_string(
    const sao_core_settings_t* settings, const char* key_utf8,
    const char* default_value_utf8,
    char* out_buffer, size_t buffer_len, size_t* out_size_needed) {
    if (settings == nullptr || key_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string* src = nullptr;
    std::string owned;

    auto it = settings->data.find(key_utf8);
    if (it != settings->data.end()) {
        if (const auto* p = std::get_if<std::string>(&it->second)) {
            src = p;
        } else {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    } else {
        owned = default_value_utf8 ? std::string(default_value_utf8) : std::string();
        src = &owned;
    }

    const size_t needed = src->size() + 1;
    if (out_size_needed != nullptr) *out_size_needed = needed;
    if (out_buffer == nullptr || buffer_len == 0) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (buffer_len < needed) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_buffer, src->data(), src->size());
    out_buffer[src->size()] = '\0';
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------
extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_set_int(
    sao_core_settings_t* settings, const char* key_utf8, int64_t value) {
    if (settings == nullptr || key_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    settings->data[key_utf8] = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_set_bool(
    sao_core_settings_t* settings, const char* key_utf8, bool value) {
    if (settings == nullptr || key_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    settings->data[key_utf8] = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_set_float(
    sao_core_settings_t* settings, const char* key_utf8, double value) {
    if (settings == nullptr || key_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    settings->data[key_utf8] = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_set_string(
    sao_core_settings_t* settings, const char* key_utf8,
    const char* value_utf8) {
    if (settings == nullptr || key_utf8 == nullptr || value_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    settings->data[key_utf8] = std::string(value_utf8);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_key_count(
    const sao_core_settings_t* settings, size_t* out_count) {
    if (settings == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_count = settings->data.size();
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Settings fixture parity helpers.
//
// Reproduce Python's ``json.dumps(data, ensure_ascii=False)`` byte-for-byte.
// The differences vs nlohmann::json::dump() that matter here:
//   * Python compact: separator between items is ``", "`` (comma + space),
//     between key/value is ``": "`` (colon + space).  nlohmann compact
//     uses ``","`` and ``":"``.
//   * Python indent=2: same ``": "`` separator; array/object braces get
//     their own line; empty {} / [] stay on one line.
//   * Python preserves dict insertion order.  nlohmann's ``ordered_json``
//     does that; the plain ``json`` alphabetises keys.
//   * Python floats use repr-style (shortest round-trip); nlohmann's
//     grisu2 writer produces the same shortest form for the values the
//     fixtures pin (0.85, 1.25).
//   * ``ensure_ascii=False`` keeps CJK verbatim; nlohmann emits verbatim
//     UTF-8 by default too, so no divergence there.
//
// The writer below walks an ``ordered_json`` value and emits the
// separators + escapes explicitly.  Only the characters Python's json
// escapes (control + " + \) are ever escaped — CJK / other high UTF-8
// bytes stream through unchanged, matching ensure_ascii=False.
// ---------------------------------------------------------------------------
namespace {

using ojson = nlohmann::ordered_json;

// Append the Python-JSON representation of a UTF-8 string, matching
// ``json.dumps(s, ensure_ascii=False)``.
void appendPythonJsonString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out.append(buf);
                } else {
                    // Non-ASCII bytes stream through verbatim; that's how
                    // Python's ensure_ascii=False writer behaves.
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

// Format a JSON number the way Python's ``json`` does: integers without a
// decimal point, floats via the shortest-round-trip form.
void appendPythonJsonNumber(std::string& out, const ojson& v) {
    if (v.is_number_integer() || v.is_number_unsigned()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(v.get<int64_t>()));
        out.append(buf);
        return;
    }
    // Float path — use nlohmann's grisu2 shortest writer.  For the pinned
    // fixtures (0.85, 1.25) this matches Python's repr byte-for-byte.
    const std::string s = v.dump();
    out.append(s);
}

// Recursive walker that emits the Python-compact form.
void writeCompact(std::string& out, const ojson& v) {
    if (v.is_null())          { out.append("null"); return; }
    if (v.is_boolean())       { out.append(v.get<bool>() ? "true" : "false"); return; }
    if (v.is_number())        { appendPythonJsonNumber(out, v); return; }
    if (v.is_string())        { appendPythonJsonString(out, v.get<std::string>()); return; }
    if (v.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : v) {
            if (!first) out.append(", ");
            first = false;
            writeCompact(out, item);
        }
        out.push_back(']');
        return;
    }
    if (v.is_object()) {
        out.push_back('{');
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out.append(", ");
            first = false;
            appendPythonJsonString(out, it.key());
            out.append(": ");
            writeCompact(out, it.value());
        }
        out.push_back('}');
        return;
    }
    // Fallback for unexpected types.
    out.append("null");
}

// Recursive walker that emits Python's ``indent=2`` form.
void writePretty(std::string& out, const ojson& v, int depth) {
    if (v.is_null())    { out.append("null"); return; }
    if (v.is_boolean()) { out.append(v.get<bool>() ? "true" : "false"); return; }
    if (v.is_number())  { appendPythonJsonNumber(out, v); return; }
    if (v.is_string())  { appendPythonJsonString(out, v.get<std::string>()); return; }

    const std::string indent(static_cast<size_t>(depth * 2 + 2), ' ');
    const std::string outdent(static_cast<size_t>(depth * 2), ' ');

    if (v.is_array()) {
        if (v.empty()) { out.append("[]"); return; }
        out.append("[\n");
        bool first = true;
        for (const auto& item : v) {
            if (!first) out.append(",\n");
            first = false;
            out.append(indent);
            writePretty(out, item, depth + 1);
        }
        out.push_back('\n');
        out.append(outdent);
        out.push_back(']');
        return;
    }
    if (v.is_object()) {
        if (v.empty()) { out.append("{}"); return; }
        out.append("{\n");
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) out.append(",\n");
            first = false;
            out.append(indent);
            appendPythonJsonString(out, it.key());
            out.append(": ");
            writePretty(out, it.value(), depth + 1);
        }
        out.push_back('\n');
        out.append(outdent);
        out.push_back('}');
        return;
    }
    out.append("null");
}

// Common tail: copy `str` into out_buf if there's room; report the needed
// size in every case.  Matches the buffer-size convention the older
// getters use in this file.
sao_status_t settings_dump_finish(const std::string& str,
                                  uint8_t* out_buf,
                                  size_t out_capacity,
                                  size_t* out_size_needed) {
    if (out_size_needed != nullptr) *out_size_needed = str.size();
    if (out_buf == nullptr) {
        return out_capacity == 0 ? SAO_STATUS_OK
                                 : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_capacity < str.size()) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    if (!str.empty()) std::memcpy(out_buf, str.data(), str.size());
    return SAO_STATUS_OK;
}

// Parse the caller-supplied JSON blob into an ``ordered_json`` value.
// Returns SAO_STATUS_ERR_INVALID_ARGUMENT on parse failure or if the
// document root isn't an object.
sao_status_t parse_settings_object(const uint8_t* input_json_utf8,
                                   size_t input_size,
                                   ojson& out) {
    if (input_json_utf8 == nullptr && input_size > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        out = ojson::parse(reinterpret_cast<const char*>(input_json_utf8),
                           reinterpret_cast<const char*>(input_json_utf8) + input_size);
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!out.is_object()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

// The single-source-of-truth defaults, mirroring python/config.py.
// DEFAULT_HOTKEYS insertion order MUST match Python so the merged output
// preserves the pinned key sequence.
const std::vector<std::pair<std::string, std::string>>& defaultHotkeys() {
    static const std::vector<std::pair<std::string, std::string>> kDefaults = {
        {"toggle_recognition",   "F5"},
        {"toggle_topmost",       "F9"},
        {"hide_panels",          "F10"},
        {"show_plugins",         "F11"},
        {"toggle_float_button",  "INSERT"},
        {"toggle_sao_menu",      "HOME"},
    };
    return kDefaults;
}

// Mirror of python/config.py::normalize_panel_theme.
std::string normalizePanelTheme(const ojson& v, const std::string& default_theme) {
    std::string fallback = "dark";
    {
        std::string dt = default_theme;
        // strip + lower
        while (!dt.empty() && (dt.front() == ' ' || dt.front() == '\t')) dt.erase(dt.begin());
        while (!dt.empty() && (dt.back()  == ' ' || dt.back()  == '\t')) dt.pop_back();
        for (auto& c : dt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (dt == "light") fallback = "light";
    }
    std::string s;
    if (v.is_string()) s = v.get<std::string>();
    // Same strip + lower.
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "light" ? std::string("light") : fallback;
}

// Mirror of python/config.py::normalize_panel_themes.
ojson normalizePanelThemes(const ojson& raw) {
    ojson themes = ojson::object();
    themes["act"] = "dark";  // DEFAULT_PANEL_THEMES
    if (raw.is_object()) {
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            std::string key = it.key();
            // Strip + lower the key.
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
            while (!key.empty() && (key.back()  == ' ' || key.back()  == '\t')) key.pop_back();
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::string existing = themes.contains(key)
                                       ? themes.at(key).get<std::string>()
                                       : std::string("dark");
            themes[key] = normalizePanelTheme(it.value(), existing);
        }
    }
    return themes;
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_dump_compact(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed) {
    ojson root;
    const sao_status_t rc = parse_settings_object(
        input_json_utf8, input_size, root);
    if (rc != SAO_STATUS_OK) return rc;
    std::string s;
    writeCompact(s, root);
    return settings_dump_finish(s, out_buf, out_capacity, out_size_needed);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_dump_pretty(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed) {
    ojson root;
    const sao_status_t rc = parse_settings_object(
        input_json_utf8, input_size, root);
    if (rc != SAO_STATUS_OK) return rc;
    std::string s;
    writePretty(s, root, 0);
    return settings_dump_finish(s, out_buf, out_capacity, out_size_needed);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_normalize_panel_themes(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed) {
    ojson root;
    const sao_status_t rc = parse_settings_object(
        input_json_utf8, input_size, root);
    if (rc != SAO_STATUS_OK) return rc;
    // ``get("panel_themes", DEFAULT_SETTINGS["panel_themes"])`` — DEFAULT
    // is {"act":"dark"}, but the SettingsManager.get() call is what feeds
    // normalize_panel_themes; when the key is missing we still normalise
    // {} (not the default), because the fixture ``partial_missing_panel_themes``
    // pins the normalised value = {"act":"dark"} which is what
    // normalize_panel_themes({}) returns anyway.
    ojson raw = ojson::object();
    if (root.contains("panel_themes")) raw = root["panel_themes"];
    ojson normalised = normalizePanelThemes(raw);
    std::string s;
    writeCompact(s, normalised);
    return settings_dump_finish(s, out_buf, out_capacity, out_size_needed);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_merge_hotkeys(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed) {
    ojson root;
    const sao_status_t rc = parse_settings_object(
        input_json_utf8, input_size, root);
    if (rc != SAO_STATUS_OK) return rc;
    ojson merged = ojson::object();
    for (const auto& kv : defaultHotkeys()) {
        merged[kv.first] = kv.second;
    }
    if (root.contains("hotkeys") && root["hotkeys"].is_object()) {
        for (auto it = root["hotkeys"].begin();
             it != root["hotkeys"].end(); ++it) {
            merged[it.key()] = it.value();
        }
    }
    std::string s;
    writeCompact(s, merged);
    return settings_dump_finish(s, out_buf, out_capacity, out_size_needed);
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_settings_strip_legacy_dump(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed) {
    ojson root;
    const sao_status_t rc = parse_settings_object(
        input_json_utf8, input_size, root);
    if (rc != SAO_STATUS_OK) return rc;
    static const char* const kLegacy[] = {
        "last_file", "speed", "transpose", "chord_mode",
    };
    // Copy in original insertion order minus the legacy keys.
    ojson stripped = ojson::object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& key = it.key();
        bool skip = false;
        for (const char* legacy : kLegacy) {
            if (key == legacy) { skip = true; break; }
        }
        if (skip) continue;
        stripped[key] = it.value();
    }
    std::string s;
    writeCompact(s, stripped);
    return settings_dump_finish(s, out_buf, out_capacity, out_size_needed);
}
