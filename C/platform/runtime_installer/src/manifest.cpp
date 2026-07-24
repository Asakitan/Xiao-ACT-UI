// SAO Auto -- runtime installer manifest parser.
//
// Hand-rolled JSON reader.  The schema is fixed and small (see
// include/sao/runtime_installer/manifest.h) so the parser only needs to
// recognise strings, integers, arrays of strings and nested objects; there
// is no need for a full RFC 8259 implementation.  Keeping the parser
// self-contained avoids growing the vcpkg dependency list for a single-use
// consumer.  The trade-off is that unrelated JSON producers are not
// accepted -- but the manifest is a private wire format between the
// launcher and the installer, so that is acceptable.
//
// Rules enforced:
//   * UTF-8 only.
//   * Total blob capped at SAO_RUNTIME_MANIFEST_MAX_JSON_BYTES.
//   * Every string capped at its schema-specific limit.
//   * Comments, trailing commas and BOMs rejected.
//   * Unknown keys ignored (forward compatibility).

#include "manifest_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "sao_security/obfuscation/enc_str.h"

namespace sao::runtime_installer::internal {

namespace {

// ---------------------------------------------------------------------------
// Tokeniser.  A tiny state machine that walks the JSON bytes; every read
// method returns SAO_STATUS_ERR_INVALID_ARGUMENT on malformed input.
// ---------------------------------------------------------------------------
class Reader {
public:
    Reader(const char* data, size_t length) noexcept
        : begin_(data), cursor_(data), end_(data + length) {}

    size_t offset() const noexcept { return static_cast<size_t>(cursor_ - begin_); }

    void skip_whitespace() noexcept {
        while (cursor_ < end_) {
            const unsigned char c = static_cast<unsigned char>(*cursor_);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++cursor_;
            } else {
                break;
            }
        }
    }

    bool accept(char expected) noexcept {
        skip_whitespace();
        if (cursor_ < end_ && *cursor_ == expected) {
            ++cursor_;
            return true;
        }
        return false;
    }

    sao_status_t expect(char expected) noexcept {
        return accept(expected) ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Read a JSON string into an output buffer.  Only the escapes the
    // manifest actually uses are supported: \" \\ \/ \n \r \t \uXXXX.
    // Anything else is a parse error.
    sao_status_t read_string(std::string& out, size_t max_bytes) noexcept {
        skip_whitespace();
        if (cursor_ >= end_ || *cursor_ != '"') {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        ++cursor_;
        out.clear();
        out.reserve(64);
        while (cursor_ < end_) {
            const char c = *cursor_++;
            if (c == '"') {
                if (out.size() > max_bytes) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                return SAO_STATUS_OK;
            }
            if (c == '\\') {
                if (cursor_ >= end_) return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const char esc = *cursor_++;
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (end_ - cursor_ < 4) return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        uint32_t code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = *cursor_++;
                            uint32_t digit = 0;
                            if (h >= '0' && h <= '9') digit = static_cast<uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') digit = static_cast<uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') digit = static_cast<uint32_t>(h - 'A' + 10);
                            else return SAO_STATUS_ERR_INVALID_ARGUMENT;
                            code = (code << 4) | digit;
                        }
                        // Only ASCII escapes actually occur in the schema; reject
                        // anything above 0x7f so we do not need a full UTF-16
                        // decoder here.
                        if (code > 0x7f) return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        out.push_back(static_cast<char>(code));
                        break;
                    }
                    default: return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                out.push_back(c);
            }
            if (out.size() > max_bytes) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    sao_status_t read_uint64(uint64_t& out) noexcept {
        skip_whitespace();
        if (cursor_ >= end_) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        // No leading '+' or '-' -- sizes are non-negative.
        if (*cursor_ < '0' || *cursor_ > '9') {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        uint64_t value = 0;
        int digits = 0;
        while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
            const uint64_t next = value * 10ull + static_cast<uint64_t>(*cursor_ - '0');
            if (next < value) return SAO_STATUS_ERR_INVALID_ARGUMENT;  // overflow
            value = next;
            ++cursor_;
            ++digits;
            if (digits > 20) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (digits == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out = value;
        return SAO_STATUS_OK;
    }

    // Peek the next non-whitespace char without consuming.
    char peek() noexcept {
        skip_whitespace();
        if (cursor_ >= end_) return '\0';
        return *cursor_;
    }

    // Skip an entire JSON value without keeping it.  Used for unknown keys.
    sao_status_t skip_value() noexcept {
        skip_whitespace();
        if (cursor_ >= end_) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const char c = *cursor_;
        if (c == '"') {
            std::string dummy;
            return read_string(dummy, SAO_RUNTIME_MANIFEST_MAX_URL_BYTES);
        }
        if (c == '{') {
            ++cursor_;
            if (accept('}')) return SAO_STATUS_OK;
            do {
                std::string key;
                if (auto s = read_string(key, SAO_RUNTIME_MANIFEST_MAX_URL_BYTES); s != SAO_STATUS_OK) return s;
                if (auto s = expect(':'); s != SAO_STATUS_OK) return s;
                if (auto s = skip_value(); s != SAO_STATUS_OK) return s;
            } while (accept(','));
            return expect('}');
        }
        if (c == '[') {
            ++cursor_;
            if (accept(']')) return SAO_STATUS_OK;
            do {
                if (auto s = skip_value(); s != SAO_STATUS_OK) return s;
            } while (accept(','));
            return expect(']');
        }
        if ((c >= '0' && c <= '9') || c == '-') {
            // Consume digits and decimal separators; the schema itself only
            // uses integers but the skipper is intentionally lenient.
            ++cursor_;
            while (cursor_ < end_) {
                const char n = *cursor_;
                if ((n >= '0' && n <= '9') || n == '.' || n == 'e' || n == 'E' || n == '+' || n == '-') {
                    ++cursor_;
                } else {
                    break;
                }
            }
            return SAO_STATUS_OK;
        }
        // true / false / null literals.
        auto match = [&](std::string_view literal) noexcept -> bool {
            if (static_cast<size_t>(end_ - cursor_) < literal.size()) return false;
            if (std::memcmp(cursor_, literal.data(), literal.size()) != 0) return false;
            cursor_ += literal.size();
            return true;
        };
        if (match("true") || match("false") || match("null")) {
            return SAO_STATUS_OK;
        }
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

private:
    const char* begin_;
    const char* cursor_;
    const char* end_;
};

// Kind name lookup.  The strings live behind SAO_ENC_STR so the plaintext
// only materialises inside the parser's stack frame during the compare.
sao_status_t parse_kind_name(std::string_view name,
                             sao_runtime_kind_t& out_kind) noexcept {
    const auto py    = SAO_ENC_STR("python3_embed");
    const auto dn    = SAO_ENC_STR("dotnet_runtime");
    const auto lu    = SAO_ENC_STR("lua54_lib");
    const auto ang   = SAO_ENC_STR("angelscript_lib");
    if (name == py.view())    { out_kind = SAO_RUNTIME_KIND_PYTHON3_EMBED;   return SAO_STATUS_OK; }
    if (name == dn.view())    { out_kind = SAO_RUNTIME_KIND_DOTNET_RUNTIME;  return SAO_STATUS_OK; }
    if (name == lu.view())    { out_kind = SAO_RUNTIME_KIND_LUA54_LIB;       return SAO_STATUS_OK; }
    if (name == ang.view())   { out_kind = SAO_RUNTIME_KIND_ANGELSCRIPT_LIB; return SAO_STATUS_OK; }
    return SAO_STATUS_ERR_NOT_FOUND;
}

sao_status_t parse_archive(std::string_view name,
                           sao_runtime_archive_t& out) noexcept {
    const auto zip = SAO_ENC_STR("zip");
    const auto raw = SAO_ENC_STR("raw");
    const auto nu  = SAO_ENC_STR("nupkg");
    if (name == zip.view()) { out = SAO_RUNTIME_ARCHIVE_ZIP;   return SAO_STATUS_OK; }
    if (name == raw.view()) { out = SAO_RUNTIME_ARCHIVE_RAW;   return SAO_STATUS_OK; }
    if (name == nu.view())  { out = SAO_RUNTIME_ARCHIVE_NUPKG; return SAO_STATUS_OK; }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

sao_status_t parse_hint(std::string_view name,
                        sao_runtime_install_hint_t& out) noexcept {
    const auto tl = SAO_ENC_STR("extract_top_level");
    const auto rt = SAO_ENC_STR("nupkg_runtime_tree");
    const auto sb = SAO_ENC_STR("single_blob");
    if (name == tl.view()) { out = SAO_RUNTIME_INSTALL_HINT_EXTRACT_TOP_LEVEL;  return SAO_STATUS_OK; }
    if (name == rt.view()) { out = SAO_RUNTIME_INSTALL_HINT_NUPKG_RUNTIME_TREE; return SAO_STATUS_OK; }
    if (name == sb.view()) { out = SAO_RUNTIME_INSTALL_HINT_SINGLE_BLOB;        return SAO_STATUS_OK; }
    out = SAO_RUNTIME_INSTALL_HINT_NONE;
    return SAO_STATUS_OK;
}

bool valid_lowercase_hex(std::string_view s) noexcept {
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

sao_status_t parse_entry(Reader& reader,
                         ManifestEntry& entry,
                         bool& out_kind_known) noexcept {
    out_kind_known = false;
    if (auto s = reader.expect('{'); s != SAO_STATUS_OK) return s;
    bool have_kind = false;
    bool have_version = false;
    bool have_url = false;
    bool have_sha = false;
    bool have_size = false;
    bool have_archive = false;
    if (reader.accept('}')) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    do {
        std::string key;
        if (auto s = reader.read_string(key, 64); s != SAO_STATUS_OK) return s;
        if (auto s = reader.expect(':'); s != SAO_STATUS_OK) return s;
        // Ordered checks against SAO_ENC_STR'd tokens so the field names
        // never sit in .rdata as plaintext.
        const auto k_kind    = SAO_ENC_STR("kind");
        const auto k_version = SAO_ENC_STR("version");
        const auto k_url     = SAO_ENC_STR("url");
        const auto k_mirror  = SAO_ENC_STR("url_mirrors");
        const auto k_sha     = SAO_ENC_STR("sha256_hex");
        const auto k_blake   = SAO_ENC_STR("blake3_hex");
        const auto k_size    = SAO_ENC_STR("size_bytes");
        const auto k_arc     = SAO_ENC_STR("archive_type");
        const auto k_hint    = SAO_ENC_STR("install_hint");
        if (key == k_kind.view()) {
            std::string value;
            if (auto s = reader.read_string(value, 64); s != SAO_STATUS_OK) return s;
            const auto rc = parse_kind_name(value, entry.kind);
            if (rc == SAO_STATUS_ERR_NOT_FOUND) {
                // Unknown kind -- keep parsing so we can skip cleanly.
                entry.kind = SAO_RUNTIME_KIND_INVALID;
            } else if (rc != SAO_STATUS_OK) {
                return rc;
            } else {
                out_kind_known = true;
            }
            have_kind = true;
        } else if (key == k_version.view()) {
            if (auto s = reader.read_string(entry.version, SAO_RUNTIME_MANIFEST_MAX_VERSION_BYTES); s != SAO_STATUS_OK) return s;
            have_version = true;
        } else if (key == k_url.view()) {
            if (auto s = reader.read_string(entry.url, SAO_RUNTIME_MANIFEST_MAX_URL_BYTES); s != SAO_STATUS_OK) return s;
            have_url = true;
        } else if (key == k_mirror.view()) {
            if (auto s = reader.expect('['); s != SAO_STATUS_OK) return s;
            entry.mirrors.clear();
            if (!reader.accept(']')) {
                do {
                    std::string mirror;
                    if (auto s = reader.read_string(mirror, SAO_RUNTIME_MANIFEST_MAX_URL_BYTES); s != SAO_STATUS_OK) return s;
                    if (entry.mirrors.size() >= SAO_RUNTIME_MANIFEST_MAX_MIRROR_COUNT) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    entry.mirrors.push_back(std::move(mirror));
                } while (reader.accept(','));
                if (auto s = reader.expect(']'); s != SAO_STATUS_OK) return s;
            }
        } else if (key == k_sha.view()) {
            if (auto s = reader.read_string(entry.sha256_hex, SAO_RUNTIME_MANIFEST_MAX_HEX_BYTES); s != SAO_STATUS_OK) return s;
            if (entry.sha256_hex.size() != 64 || !valid_lowercase_hex(entry.sha256_hex)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            have_sha = true;
        } else if (key == k_blake.view()) {
            if (auto s = reader.read_string(entry.blake3_hex, SAO_RUNTIME_MANIFEST_MAX_HEX_BYTES); s != SAO_STATUS_OK) return s;
            if (!entry.blake3_hex.empty() &&
                (entry.blake3_hex.size() != 64 || !valid_lowercase_hex(entry.blake3_hex))) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        } else if (key == k_size.view()) {
            if (auto s = reader.read_uint64(entry.size_bytes); s != SAO_STATUS_OK) return s;
            if (entry.size_bytes == 0 || entry.size_bytes > SAO_RUNTIME_MANIFEST_MAX_PAYLOAD_BYTES) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            have_size = true;
        } else if (key == k_arc.view()) {
            std::string value;
            if (auto s = reader.read_string(value, 16); s != SAO_STATUS_OK) return s;
            if (auto s = parse_archive(value, entry.archive); s != SAO_STATUS_OK) return s;
            have_archive = true;
        } else if (key == k_hint.view()) {
            std::string value;
            if (auto s = reader.read_string(value, 32); s != SAO_STATUS_OK) return s;
            if (auto s = parse_hint(value, entry.install_hint); s != SAO_STATUS_OK) return s;
        } else {
            // Unknown key -- skip its value to preserve forward compat.
            if (auto s = reader.skip_value(); s != SAO_STATUS_OK) return s;
        }
    } while (reader.accept(','));
    if (auto s = reader.expect('}'); s != SAO_STATUS_OK) return s;
    if (!have_kind || !have_version || !have_url || !have_sha || !have_size || !have_archive) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

}  // namespace

sao_status_t parse_manifest(const char* data,
                            size_t length,
                            Manifest& out) noexcept {
    if (data == nullptr || length == 0 ||
        length > SAO_RUNTIME_MANIFEST_MAX_JSON_BYTES) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Reject BOM.  The wire format is ASCII-friendly UTF-8 without one.
    if (length >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xef &&
        static_cast<unsigned char>(data[1]) == 0xbb &&
        static_cast<unsigned char>(data[2]) == 0xbf) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    Reader reader(data, length);
    if (auto s = reader.expect('{'); s != SAO_STATUS_OK) return s;
    bool have_schema = false;
    bool have_version = false;
    bool have_entries = false;
    if (reader.accept('}')) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    do {
        std::string key;
        if (auto s = reader.read_string(key, 32); s != SAO_STATUS_OK) return s;
        if (auto s = reader.expect(':'); s != SAO_STATUS_OK) return s;
        const auto k_schema  = SAO_ENC_STR("schema");
        const auto k_version = SAO_ENC_STR("version");
        const auto k_entries = SAO_ENC_STR("entries");
        if (key == k_schema.view()) {
            uint64_t schema = 0;
            if (auto s = reader.read_uint64(schema); s != SAO_STATUS_OK) return s;
            if (schema < SAO_RUNTIME_INSTALLER_MANIFEST_SCHEMA_MIN ||
                schema > SAO_RUNTIME_INSTALLER_MANIFEST_SCHEMA_MAX) {
                return SAO_STATUS_ERR_ABI_MISMATCH;
            }
            out.schema = static_cast<uint32_t>(schema);
            have_schema = true;
        } else if (key == k_version.view()) {
            if (auto s = reader.read_string(out.version, SAO_RUNTIME_MANIFEST_MAX_VERSION_BYTES); s != SAO_STATUS_OK) return s;
            have_version = true;
        } else if (key == k_entries.view()) {
            if (auto s = reader.expect('['); s != SAO_STATUS_OK) return s;
            if (!reader.accept(']')) {
                do {
                    if (out.entries.size() >= SAO_RUNTIME_MANIFEST_MAX_ENTRIES) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    ManifestEntry entry{};
                    bool known = false;
                    if (auto s = parse_entry(reader, entry, known); s != SAO_STATUS_OK) return s;
                    if (known) {
                        // Reject duplicates -- ambiguous which URL wins.
                        for (const auto& existing : out.entries) {
                            if (existing.kind == entry.kind) {
                                return SAO_STATUS_ERR_ALREADY_EXISTS;
                            }
                        }
                        out.entries.push_back(std::move(entry));
                    }
                } while (reader.accept(','));
                if (auto s = reader.expect(']'); s != SAO_STATUS_OK) return s;
            }
            have_entries = true;
        } else {
            if (auto s = reader.skip_value(); s != SAO_STATUS_OK) return s;
        }
    } while (reader.accept(','));
    if (auto s = reader.expect('}'); s != SAO_STATUS_OK) return s;
    reader.skip_whitespace();
    if (!have_schema || !have_version || !have_entries || out.entries.empty()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

const ManifestEntry* find_entry(const Manifest& manifest,
                                sao_runtime_kind_t kind) noexcept {
    for (const auto& entry : manifest.entries) {
        if (entry.kind == kind) return &entry;
    }
    return nullptr;
}

}  // namespace sao::runtime_installer::internal

// ---------------------------------------------------------------------------
// C ABI thunks that read individual fields off a parsed manifest.  These are
// intentionally thin -- the real work happens in the parser above.
// ---------------------------------------------------------------------------

namespace {

sao_status_t copy_out(const std::string& src,
                      char* out_utf8,
                      size_t capacity,
                      size_t* out_required) noexcept {
    const size_t required = src.size() + 1;
    if (out_required != nullptr) *out_required = required;
    if (out_utf8 == nullptr || capacity == 0) {
        return capacity == 0 && out_utf8 == nullptr
            ? SAO_STATUS_OK
            : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (capacity < required) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_utf8, src.data(), src.size());
    out_utf8[src.size()] = '\0';
    return SAO_STATUS_OK;
}

sao::runtime_installer::internal::Manifest*
handle_to_manifest(sao_runtime_manifest_handle_t handle) noexcept {
    return reinterpret_cast<sao::runtime_installer::internal::Manifest*>(handle);
}

}  // namespace

extern "C" {

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_url(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*                         out_utf8,
    size_t                        capacity,
    size_t*                       out_required) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    return copy_out(entry->url, out_utf8, capacity, out_required);
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_version(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*                         out_utf8,
    size_t                        capacity,
    size_t*                       out_required) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    return copy_out(entry->version, out_utf8, capacity, out_required);
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_sha256_hex(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*                         out_utf8,
    size_t                        capacity,
    size_t*                       out_required) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    return copy_out(entry->sha256_hex, out_utf8, capacity, out_required);
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_archive(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    sao_runtime_archive_t*        out_archive) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_archive == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    *out_archive = entry->archive;
    return SAO_STATUS_OK;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_install_hint(
    sao_runtime_manifest_handle_t   handle,
    sao_runtime_kind_t              kind,
    sao_runtime_install_hint_t*     out_hint) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hint == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    *out_hint = entry->install_hint;
    return SAO_STATUS_OK;
}

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_size(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    uint64_t*                     out_bytes) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_bytes == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto* manifest = handle_to_manifest(handle);
    const auto* entry = sao::runtime_installer::internal::find_entry(*manifest, kind);
    if (entry == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    *out_bytes = entry->size_bytes;
    return SAO_STATUS_OK;
}

}  // extern "C"
