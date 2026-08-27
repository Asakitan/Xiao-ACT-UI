#include "sao/plugins/loader/plugin_manifest.h"
#include "plugin_internal.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

#include <windows.h>

namespace sao::plugins::loader {
namespace {

using json = nlohmann::ordered_json;

class bounded_manifest_json_sax final : public json::json_sax_t {
  public:
    bool null() override {
        return consume_node();
    }
    bool boolean(bool) override {
        return consume_node();
    }
    bool number_integer(number_integer_t) override {
        return consume_node();
    }
    bool number_unsigned(number_unsigned_t value) override {
        return value <= static_cast<number_unsigned_t>((std::numeric_limits<int64_t>::max)()) &&
               consume_node();
    }
    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && consume_node();
    }
    bool string(string_t& value) override {
        return consume_string(value) && consume_node();
    }
    bool binary(binary_t&) override {
        return consume_node();
    }
    bool start_object(std::size_t) override {
        return start_container();
    }
    bool key(string_t& value) override {
        return consume_string(value);
    }
    bool end_object() override {
        return end_container();
    }
    bool start_array(std::size_t) override {
        return start_container();
    }
    bool end_array() override {
        return end_container();
    }
    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    bool consume_node() noexcept {
        if (nodes_ >= kMaximumManifestJsonNodes)
            return false;
        ++nodes_;
        return true;
    }

    bool consume_string(const string_t& value) noexcept {
        if (value.find('\0') != string_t::npos || value.size() > kMaximumManifestStringBytes ||
            string_bytes_ > kMaximumManifestAggregateStringBytes ||
            value.size() > kMaximumManifestAggregateStringBytes - string_bytes_) {
            return false;
        }
        string_bytes_ += value.size();
        return true;
    }
    bool start_container() noexcept {
        if (depth_ >= kMaximumManifestJsonDepth || !consume_node())
            return false;
        ++depth_;
        return true;
    }

    bool end_container() noexcept {
        if (depth_ == 0)
            return false;
        --depth_;
        return true;
    }

    size_t depth_ = 0;
    size_t nodes_ = 0;
    size_t string_bytes_ = 0;
};

std::wstring normalized_final_path(HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0)
        return {};
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), required, flags);
    if (written == 0 || written >= required)
        return {};
    std::wstring result(buffer.data(), written);
    constexpr std::wstring_view kUncPrefix = LR"(\\?\UNC\)";
    constexpr std::wstring_view kDosPrefix = LR"(\\?\)";
    if (result.starts_with(kUncPrefix)) {
        result = LR"(\\)" + result.substr(kUncPrefix.size());
    } else if (result.starts_with(kDosPrefix)) {
        result.erase(0, kDosPrefix.size());
    }
    return result;
}

bool resolve_existing_path(const std::filesystem::path& path,
                           std::filesystem::path& output) noexcept {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const auto final_path = normalized_final_path(handle);
    CloseHandle(handle);
    if (final_path.empty())
        return false;
    output = std::filesystem::path(final_path).lexically_normal();
    return true;
}

bool same_path_component(const std::filesystem::path& left,
                         const std::filesystem::path& right) noexcept {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) noexcept {
    auto root_iterator = root.begin();
    auto candidate_iterator = candidate.begin();
    for (; root_iterator != root.end(); ++root_iterator, ++candidate_iterator) {
        if (candidate_iterator == candidate.end() ||
            !same_path_component(*root_iterator, *candidate_iterator)) {
            return false;
        }
    }
    return true;
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto& wide = path.native();
    if (wide.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                        static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string string_value(const json& object, const char* key) {
    const auto iterator = object.find(key);
    return iterator != object.end() && iterator->is_string()
        ? iterator->get<std::string>() : std::string{};
}

std::vector<std::string> string_array(const json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string() && !item.get_ref<const std::string&>().empty()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

void parse_requires(const json& value, std::vector<std::string>& result) {
    auto trim = [](std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    };
    auto quote = [](std::string_view text) {
        const char quote_character = text.find('\'') != std::string_view::npos and
                                             text.find('"') == std::string_view::npos ? '"' : '\'';
        std::string output(1, quote_character);
        for (const char character : text) {
            const auto byte = static_cast<unsigned char>(character);
            if (character == '\\') output += "\\\\";
            else if (character == quote_character) { output += "\\"; output.push_back(quote_character); }
            else if (character == '\n') output += "\\n";
            else if (character == '\r') output += "\\r";
            else if (character == '\t') output += "\\t";
            else if (byte < 0x20U or byte == 0x7fU) {
                char escaped[5]{};
                std::snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
                output += escaped;
            } else output.push_back(character);
        }
        output.push_back(quote_character);
        return output;
    };
    std::function<std::string(const json&, bool)> stringify =
        [&](const json& item, bool nested) -> std::string {
        if (item.is_string()) return nested ? quote(item.get<std::string>()) : item.get<std::string>();
        if (item.is_boolean()) return item.get<bool>() ? "True" : "False";
        if (item.is_number_integer()) return std::to_string(item.get<int64_t>());
        if (item.is_number_unsigned()) return std::to_string(item.get<uint64_t>());
        if (item.is_number_float()) {
            char buffer[64]{};
            const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), item.get<double>());
            if (converted.ec != std::errc{}) return {};
            std::string output(buffer, converted.ptr);
            if (output.find_first_of(".eE") == std::string::npos) output += ".0";
            return output;
        }
        if (item.is_null()) return "None";
        if (item.is_array()) {
            std::string output = "[";
            bool first = true;
            for (const auto& child : item) {
                if (!first) output += ", ";
                output += stringify(child, true);
                first = false;
            }
            output += "]";
            return output;
        }
        std::string output = "{";
        bool first = true;
        for (const auto& [key, child] : item.items()) {
            if (!first) output += ", ";
            output += quote(key) + ": " + stringify(child, true);
            first = false;
        }
        output += "}";
        return output;
    };
    auto scalar = [&](const json& item) { return stringify(item, false); };
    auto truthy = [](const json& item) {
        if (item.is_null()) return false;
        if (item.is_boolean()) return item.get<bool>();
        if (item.is_number_integer()) return item.get<int64_t>() != 0;
        if (item.is_number_unsigned()) return item.get<uint64_t>() != 0;
        if (item.is_number_float()) return item.get<double>() != 0.0;
        if (item.is_string()) return !item.get_ref<const std::string&>().empty();
        return !item.empty();
    };
    auto append = [&](std::string text) {
        text = trim(std::move(text));
        if (!text.empty()) result.push_back(std::move(text));
    };
    if (value.is_array()) {
        for (const auto& item : value) if (truthy(item)) append(scalar(item));
        return;
    }
    if (value.is_null() || value.is_boolean() || value.is_number() || value.is_string()) {
        if (truthy(value)) append(scalar(value));
        return;
    }
    if (!value.is_object()) return;
    for (const auto& [raw_key, item] : value.items()) {
        const auto key = trim(raw_key);
        if (key.empty()) continue;
        if (key == "runtime_features" && item.is_array()) {
            for (const auto& feature : item) if (truthy(feature)) append("runtime_feature:" + scalar(feature));
        } else if (item.is_boolean() && item.get<bool>()) {
            append(key);
        } else if (truthy(item)) {
            append(key + scalar(item));
        }
    }
}

void parse_capabilities(const json& value, std::vector<capability_entry>& result) {
    if (!value.is_array()) return;
    for (const auto& item : value) {
        capability_entry entry;
        if (item.is_string()) {
            entry.id = item.get<std::string>();
            entry.title = entry.id;
        } else if (item.is_object()) {
            entry.id = string_value(item, "id");
            if (entry.id.empty()) entry.id = string_value(item, "capability_id");
            entry.title = string_value(item, "title");
            entry.description = string_value(item, "description");
            entry.route = string_value(item, "route");
            entry.render_hint = string_value(item, "render_hint");
            if (const auto iterator = item.find("actions"); iterator != item.end()) {
                entry.actions = string_array(*iterator);
            }
            if (const auto iterator = item.find("payload_fields"); iterator != item.end()) {
                entry.payload_fields = string_array(*iterator);
            }
            if (entry.title.empty()) entry.title = entry.id;
        }
        if (!entry.id.empty()) result.push_back(std::move(entry));
    }
}

void parse_hotkeys(const json& value, std::vector<hotkey_entry>& result) {
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            hotkey_entry entry;
            entry.hotkey_id = key;
            if (item.is_string()) entry.default_key = item.get<std::string>();
            else if (item.is_object()) {
                entry.default_key = string_value(item, "default");
                if (entry.default_key.empty()) entry.default_key = string_value(item, "default_key");
                entry.label = string_value(item, "label");
            }
            if (!entry.default_key.empty()) result.push_back(std::move(entry));
        }
        return;
    }
    if (!value.is_array()) return;
    for (const auto& item : value) {
        if (!item.is_object()) continue;
        hotkey_entry entry;
        entry.hotkey_id = string_value(item, "id");
        if (entry.hotkey_id.empty()) entry.hotkey_id = string_value(item, "hotkey_id");
        entry.default_key = string_value(item, "default");
        if (entry.default_key.empty()) entry.default_key = string_value(item, "default_key");
        entry.label = string_value(item, "label");
        if (!entry.hotkey_id.empty()) result.push_back(std::move(entry));
    }
}

void parse_settings(const json& value, std::vector<settings_schema_entry>& result) {
    if (!value.is_object()) return;
    for (const auto& [key, item] : value.items()) {
        if (!item.is_object()) continue;
        settings_schema_entry entry;
        entry.key = key;
        entry.type = string_value(item, "type");
        entry.description = string_value(item, "description");
        if (const auto iterator = item.find("default"); iterator != item.end()) {
            entry.default_json = iterator->dump();
        }
        result.push_back(std::move(entry));
    }
}

bool valid_plugin_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
        }) && value != "." && value != "..";
}

bool valid_relative_entry(std::string_view value) {
    if (value.empty() || value.size() > 1024) return false;
    const std::filesystem::path path = std::filesystem::u8path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

int32_t populate_manifest(const json& root, plugin_manifest& output) {
    if (!root.is_object()) return SAO_ERR_INVALID_ARGUMENT;
    output.plugin_id = string_value(root, "id");
    output.name = string_value(root, "name");
    output.version = string_value(root, "version");
    output.description = string_value(root, "description");
    output.entry = string_value(root, "entry");
    output.managed_type = string_value(root, "managed_type");
    output.runtimeconfig = string_value(root, "runtimeconfig");
    output.enabled = root.value("enabled", false);
    output.native_entry = string_value(root, "native_entry");
    output.native_abi = string_value(root, "native_abi");
    output.abi_version = root.value("abi_version", 0U);

    const bool native_only = output.entry.empty() && !output.native_entry.empty();
    if (native_only) output.entry = output.native_entry;

    auto language = string_value(root, "language");
    if (language.empty()) language = string_value(root, "engine");
    if (language.empty()) language = string_value(root, "runtime");
    output.language = parse_engine_kind(language);
    if (output.language == engine_kind::unknown) output.language = infer_language_from_entry(output.entry);
    if (output.language == engine_kind::unknown && output.entry.empty() &&
        output.native_entry.empty()) {
        output.language = engine_kind::python;
    }
    if (output.entry.empty()) output.entry = guess_default_entry_for(output.language);
    if (output.name.empty()) output.name = output.plugin_id;
    if (output.version.empty()) output.version = "0.1.0";

    if (const auto iterator = root.find("requires"); iterator != root.end()) {
        parse_requires(*iterator, output.requires_list);
    }
    if (const auto iterator = root.find("deps"); iterator != root.end()) {
        std::vector<std::string> dependencies;
        parse_requires(*iterator, dependencies);
        output.requires_list.insert(output.requires_list.end(), dependencies.begin(), dependencies.end());
    }
    if (const auto iterator = root.find("permissions"); iterator != root.end()) output.permissions = string_array(*iterator);
    if (const auto iterator = root.find("game_ids"); iterator != root.end()) output.game_ids = string_array(*iterator);
    if (const auto iterator = root.find("capabilities"); iterator != root.end()) parse_capabilities(*iterator, output.capabilities);
    if (const auto iterator = root.find("hotkeys"); iterator != root.end()) parse_hotkeys(*iterator, output.hotkeys);
    if (const auto iterator = root.find("settings_schema"); iterator != root.end()) parse_settings(*iterator, output.settings_schema);
    if (const auto iterator = root.find("sao_menu"); iterator != root.end()) output.sao_menu_json = iterator->dump();
    for (const auto* key : {"locales", "i18n", "translations"}) {
        if (const auto iterator = root.find(key); iterator != root.end() && iterator->is_object()) {
            output.locales_json = iterator->dump();
            if (!output.locales_json.empty()) break;
        }
    }
    output.primary = root.value("primary", true);
    output.hidden = root.value("hidden", false);
    output.min_width = root.value("min_width", 0U);
    output.min_height = root.value("min_height", 0U);
    if (const auto iterator = root.find("mcpServers"); iterator != root.end()) output.mcp_servers_json = iterator->dump();
    if (const auto iterator = root.find("chatProviders"); iterator != root.end()) output.chat_providers_json = iterator->dump();
    output.protected_plugin = root.value("protected", false);
    return SAO_OK;
}

} // namespace

int32_t resolve_contained_existing_path(const std::filesystem::path& root,
                                        const std::filesystem::path& candidate,
                                        std::filesystem::path& out_resolved) noexcept {
    out_resolved.clear();
    try {
        std::filesystem::path resolved_root;
        std::filesystem::path resolved_candidate;
        if (!resolve_existing_path(root, resolved_root) ||
            !resolve_existing_path(candidate, resolved_candidate) ||
            !path_is_within(resolved_root, resolved_candidate)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        out_resolved = std::move(resolved_candidate);
        return SAO_OK;
    } catch (...) {
        out_resolved.clear();
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_manifest_parse(const char* utf8_json_ptr,
                           size_t utf8_json_len,
                           plugin_manifest* out_manifest) {
    if (out_manifest == nullptr || utf8_json_ptr == nullptr || utf8_json_len == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_manifest = plugin_manifest{};
    if (utf8_json_len > kMaximumManifestRawBytes) {
        out_manifest->parse_error = "manifest exceeds raw byte budget";
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        const char* begin = utf8_json_ptr;
        if (utf8_json_len >= 3 && static_cast<unsigned char>(begin[0]) == 0xef &&
            static_cast<unsigned char>(begin[1]) == 0xbb &&
            static_cast<unsigned char>(begin[2]) == 0xbf) begin += 3;
        const size_t input_size = static_cast<size_t>(utf8_json_ptr + utf8_json_len - begin);
        if (input_size > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, begin, static_cast<int>(input_size),
                                   nullptr, 0) <= 0) {
            out_manifest->parse_error = "manifest JSON is not valid UTF-8";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        bounded_manifest_json_sax sax;
        if (!json::sax_parse(begin, utf8_json_ptr + utf8_json_len, &sax)) {
            out_manifest->parse_error = "manifest exceeds JSON budget or is malformed";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto root = json::parse(begin, utf8_json_ptr + utf8_json_len);
        plugin_manifest candidate;
        const int32_t status = populate_manifest(root, candidate);
        if (status != SAO_OK) {
            out_manifest->parse_error = "manifest root must be an object";
            return status;
        }
        *out_manifest = std::move(candidate);
        return SAO_OK;
    } catch (const std::exception& error) {
        out_manifest->parse_error = error.what();
        return SAO_ERR_INVALID_ARGUMENT;
    } catch (...) {
        out_manifest->parse_error = "manifest parse failed";
        return SAO_ERR_INVALID_ARGUMENT;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_manifest_load_from_file(const wchar_t* manifest_path,
                                    plugin_manifest* out_manifest) {
    if (manifest_path == nullptr || out_manifest == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_manifest = plugin_manifest{};
    try {
        const auto path = std::filesystem::path(manifest_path);
        const auto path_root = path.has_parent_path() ? path.parent_path()
                                                      : std::filesystem::current_path();
        std::filesystem::path resolved_path;
        const int32_t containment_status =
            resolve_contained_existing_path(path_root, path, resolved_path);
        if (containment_status != SAO_OK)
            return containment_status;
        std::error_code error;
        const auto file_size = std::filesystem::file_size(resolved_path, error);
        if (error)
            return SAO_ERR_HANDLE_INVALID;
        if (file_size > kMaximumManifestRawBytes) {
            out_manifest->parse_error = "manifest exceeds raw byte budget";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::ifstream input(resolved_path, std::ios::binary);
        if (!input) return SAO_ERR_HANDLE_INVALID;
        std::string content(static_cast<size_t>(file_size), '\0');
        if (file_size > 0) {
            input.read(content.data(), static_cast<std::streamsize>(file_size));
            if (input.gcount() != static_cast<std::streamsize>(file_size))
                return SAO_ERR_OS_CALL_FAILED;
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            out_manifest->parse_error = "manifest changed while being read";
            return SAO_ERR_INVALID_ARGUMENT;
        }
        plugin_manifest candidate;
        const int32_t status =
            sao_plugins_manifest_parse(content.data(), content.size(), &candidate);
        if (status != SAO_OK) {
            *out_manifest = std::move(candidate);
            return status;
        }
        const auto plugin_root = resolved_path.parent_path();
        for (const auto& entry : {candidate.entry, candidate.native_entry, candidate.runtimeconfig}) {
            if (entry.empty())
                continue;
            if (!valid_relative_entry(entry)) {
                out_manifest->parse_error = "manifest entry path is invalid";
                return SAO_ERR_INVALID_ARGUMENT;
            }
            const auto entry_path = plugin_root / std::filesystem::u8path(entry);
            error.clear();
            if (!std::filesystem::exists(entry_path, error)) {
                if (error)
                    return SAO_ERR_OS_CALL_FAILED;
                continue;
            }
            std::filesystem::path resolved_entry;
            const int32_t entry_status =
                resolve_contained_existing_path(plugin_root, entry_path, resolved_entry);
            if (entry_status != SAO_OK)
                return entry_status;
        }
        candidate.source_path = path_utf8(plugin_root);
        if (candidate.source_path.empty())
            return SAO_ERR_OS_CALL_FAILED;
        *out_manifest = std::move(candidate);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t validate_manifest(const plugin_manifest& manifest) {
    if (!manifest.parse_error.empty() || !valid_plugin_id(manifest.plugin_id) ||
        manifest.name.empty() || manifest.version.empty() ||
        (manifest.language == engine_kind::unknown && manifest.native_entry.empty()) ||
        !valid_relative_entry(manifest.entry)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (manifest.abi_version > static_cast<uint32_t>(SAO_PLUGINS_ABI_VERSION)) return SAO_ERR_INVALID_ARGUMENT;
    if (manifest.protected_plugin && manifest.native_entry.empty()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (manifest.managed_type.size() > 1024 ||
        (!manifest.runtimeconfig.empty() &&
         !valid_relative_entry(manifest.runtimeconfig))) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!manifest.native_entry.empty()) {
        if (!valid_relative_entry(manifest.native_entry) ||
            (manifest.native_abi != "sao_plugin_v1" && manifest.native_abi != "sao_plugin_v2")) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const uint32_t declared_native_abi = manifest.native_abi == "sao_plugin_v2" ? 2U : 1U;
        if (manifest.abi_version != 0 && manifest.abi_version != declared_native_abi) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    return SAO_OK;
}

engine_kind parse_engine_kind(std::string_view name) {
    std::string normalized(name);
    const auto first = normalized.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) normalized.clear();
    else normalized = normalized.substr(first, normalized.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](char ch) {
        return ch == '-' || ch == '_';
    }), normalized.end());
    if (normalized == "python" || normalized == "py") return engine_kind::python;
    if (normalized == "emma") return engine_kind::emma;
    if (normalized == "angelscript" || normalized == "as" || normalized == "angel") return engine_kind::angelscript;
    if (normalized == "lua") return engine_kind::lua;
    if (normalized == "csharp" || normalized == "cs" || normalized == "c#" ||
        normalized == ".net" || normalized == "dotnet") return engine_kind::csharp;
    return engine_kind::unknown;
}
std::string_view engine_kind_name(engine_kind kind) {
    switch (kind) {
        case engine_kind::python: return "python";
        case engine_kind::emma: return "emma";
        case engine_kind::angelscript: return "angelscript";
        case engine_kind::lua: return "lua";
        case engine_kind::csharp: return "csharp";
        default: return "unknown";
    }
}

std::string_view guess_default_entry_for(engine_kind kind) {
    switch (kind) {
        case engine_kind::python: return "plugin.py";
        case engine_kind::emma: return "plugin.emma";
        case engine_kind::angelscript: return "plugin.as";
        case engine_kind::lua: return "plugin.lua";
        case engine_kind::csharp: return "plugin.dll";
        default: return "plugin.py";
    }
}

engine_kind infer_language_from_entry(std::string_view entry_path) {
    const auto extension = lower_ascii(std::filesystem::u8path(entry_path).extension().string());
    if (extension == ".py") return engine_kind::python;
    if (extension == ".emma") return engine_kind::emma;
    if (extension == ".as") return engine_kind::angelscript;
    if (extension == ".lua") return engine_kind::lua;
    if (extension == ".cs" || extension == ".dll") return engine_kind::csharp;
    return engine_kind::unknown;
}

} // namespace sao::plugins::loader
