#include "sao/launcher/provider_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace sao::launcher {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::mutex g_configuration_mutex;
LauncherProviderConfiguration g_configuration;

bool decodeHex(std::string_view input, uint8_t* output, size_t output_size) {
    if (input.size() != output_size * 2) return false;
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < output_size; ++index) {
        const int high = nibble(input[index * 2]);
        const int low = nibble(input[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool sectionEnabled(const json& section) {
    const auto enabled = section.find("enabled");
    return enabled == section.end() ? true : enabled->get<bool>();
}

fs::path resolvePath(const fs::path& base, const std::string& value) {
    auto path = fs::u8path(value);
    if (path.is_relative()) path = base / path;
    return path.lexically_normal();
}

bool parsePathArray(const json& section, const char* name,
                    const fs::path& base, std::vector<std::wstring>& output) {
    const auto values = section.find(name);
    if (values == section.end()) return true;
    if (!values->is_array()) return false;
    for (const auto& value : *values) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            return false;
        }
        output.push_back(resolvePath(base, value.get<std::string>()).wstring());
    }
    return true;
}

bool parseShell(const json& root, const fs::path& base,
                ShellProviderConfiguration& output) {
    const auto section = root.find("shell");
    if (section == root.end()) return true;
    if (!section->is_object()) return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled) return true;
    const auto metadata = section->find("metadata");
    if (metadata == section->end() || !metadata->is_string() ||
        metadata->get_ref<const std::string&>().empty()) {
        return false;
    }
    output.metadata_path = resolvePath(base, metadata->get<std::string>()).wstring();
    return true;
}

bool parseLicense(const json& root, LicenseProviderConfiguration& output) {
    const auto section = root.find("license");
    if (section == root.end()) return true;
    if (!section->is_object()) return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled) return true;
    const auto endpoint = section->find("endpoint");
    if (endpoint == section->end() || !endpoint->is_string() ||
        endpoint->get_ref<const std::string&>().empty()) {
        return false;
    }
    output.endpoint = endpoint->get<std::string>();
    output.build_id = section->value("build_id", std::string{});
    if (section->value("responses_prevalidated", false)) return false;
    output.responses_prevalidated = false;
    output.heartbeat_interval_ms = section->value("heartbeat_interval_ms", 0U);
    const auto public_key = section->find("server_ed25519_pubkey");
    return public_key != section->end() && public_key->is_string() &&
        decodeHex(public_key->get_ref<const std::string&>(),
                  output.server_public_key.data(), output.server_public_key.size());
}

bool parsePlugins(const json& root, const fs::path& base,
                  PluginsProviderConfiguration& output) {
    const auto section = root.find("plugins");
    if (section == root.end()) return true;
    if (!section->is_object()) return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled) return true;
    output.workspace_walkup = section->value("workspace_walkup", false);
    output.max_depth = section->value("max_depth", 1U);
    if (output.max_depth == 0) return false;
    if (!parsePathArray(*section, "roots", base, output.roots) ||
        !parsePathArray(*section, "user_roots", base, output.user_roots) ||
        !parsePathArray(*section, "manifests", base, output.manifests)) {
        return false;
    }
    return !output.roots.empty() || !output.user_roots.empty() ||
        !output.manifests.empty();
}

} // namespace

sao_status_t loadLauncherProviderConfiguration(
    const wchar_t* base_dir,
    const wchar_t* config_path) noexcept {
    LauncherProviderConfiguration next;
    if (config_path == nullptr || config_path[0] == L'\0') {
        std::lock_guard lock(g_configuration_mutex);
        g_configuration = std::move(next);
        return SAO_STATUS_OK;
    }
    try {
        fs::path path(config_path);
        if (path.is_relative()) {
            if (base_dir == nullptr || base_dir[0] == L'\0') {
                return SAO_STATUS_INVALID_ARGUMENT;
            }
            path = fs::path(base_dir) / path;
        }
        path = path.lexically_normal();
        std::ifstream input(path, std::ios::binary);
        if (!input) return SAO_STATUS_INVALID_ARGUMENT;
        const auto root = json::parse(input);
        if (!root.is_object() ||
            !parseShell(root, path.parent_path(), next.shell) ||
            !parseLicense(root, next.license) ||
            !parsePlugins(root, path.parent_path(), next.plugins)) {
            return SAO_STATUS_INVALID_ARGUMENT;
        }
        // The repository has no serialized contract that can reconstruct
        // sao_shell_stub_runtime_provider_t callbacks and process-relative
        // region pointers.  A metadata filename is therefore an unsupported
        // configuration, not a successfully installed runtime provider.
        if (next.shell.enabled) return SAO_STATUS_NOT_IMPLEMENTED;
        std::lock_guard lock(g_configuration_mutex);
        g_configuration = std::move(next);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
}

LauncherProviderConfiguration launcherProviderConfigurationSnapshot() {
    std::lock_guard lock(g_configuration_mutex);
    return g_configuration;
}

} // namespace sao::launcher