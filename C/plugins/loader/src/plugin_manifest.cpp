#include "sao/plugins/loader/plugin_manifest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <windows.h>

namespace sao::plugins::loader {
namespace {

using json = nlohmann::json;

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
    if (value.is_array()) {
        result = string_array(value);
        return;
    }
    if (!value.is_object()) return;
    for (const auto& [key, item] : value.items()) {
        if (item.is_boolean()) {
            if (item.get<bool>()) result.push_back(key);
        } else if (item.is_string()) {
            result.push_back(key + item.get<std::string>());
        } else if (item.is_array()) {
            const auto prefix = key == "runtime_features" ? "runtime_feature" : key;
            for (const auto& entry : item) {
                if (entry.is_string()) result.push_back(prefix + ":" + entry.get<std::string>());
            }
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
            if (item.is_string()) result.push_back({key, item.get<std::string>(), key});
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
        if (const auto iterator = root.find(key); iterator != root.end()) {
            output.locales_json = iterator->dump();
            break;
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_manifest_parse(const char* utf8_json_ptr,
                           size_t utf8_json_len,
                           plugin_manifest* out_manifest) {
    if (out_manifest == nullptr || utf8_json_ptr == nullptr || utf8_json_len == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_manifest = plugin_manifest{};
    try {
        auto begin = utf8_json_ptr;
        if (utf8_json_len >= 3 && static_cast<unsigned char>(begin[0]) == 0xef &&
            static_cast<unsigned char>(begin[1]) == 0xbb &&
            static_cast<unsigned char>(begin[2]) == 0xbf) begin += 3;
        const auto root = json::parse(begin, utf8_json_ptr + utf8_json_len);
        return populate_manifest(root, *out_manifest);
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
        std::ifstream input(path, std::ios::binary);
        if (!input) return SAO_ERR_HANDLE_INVALID;
        std::ostringstream stream;
        stream << input.rdbuf();
        const auto content = stream.str();
        const auto status = sao_plugins_manifest_parse(content.data(), content.size(), out_manifest);
        if (status == SAO_OK) out_manifest->source_path = path_utf8(path.parent_path());
        return status;
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
    const auto normalized = lower_ascii(name);
    if (normalized == "python" || normalized == "py") return engine_kind::python;
    if (normalized == "emma") return engine_kind::emma;
    if (normalized == "angelscript" || normalized == "angel-script" || normalized == "as") return engine_kind::angelscript;
    if (normalized == "lua") return engine_kind::lua;
    if (normalized == "csharp" || normalized == "c-sharp" || normalized == "cs" ||
        normalized == "c#" || normalized == ".net" || normalized == "dotnet") return engine_kind::csharp;
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
        default: return {};
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
