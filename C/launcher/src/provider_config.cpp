#include "sao/launcher/provider_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace sao::launcher {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr uint32_t kDefaultHeartbeatIntervalMs = 300000U;
constexpr std::string_view kNativeUpdateManifestUrl =
    "https://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json";
constexpr std::string_view kNativeUpdateTlsSpkiSha256 =
    "d3171ec5b86303233b6abda8e83142293d6910099aedd0cdac947d276006db0a";

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

bool isAllZero(const std::array<uint8_t, 32>& value) {
    uint8_t aggregate = 0;
    for (const uint8_t byte : value) aggregate |= byte;
    return aggregate == 0;
}

bool equalBytes(const std::array<uint8_t, 32>& left,
                const std::array<uint8_t, 32>& right) {
    uint8_t difference = 0;
    for (size_t index = 0; index < left.size(); ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0;
}

bool isPrintableAscii(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20u && ch <= 0x7eu;
    });
}

bool validPort(std::string_view value) {
    if (value.empty() || value.size() > 5) return false;
    uint32_t port = 0;
    for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) return false;
        port = port * 10u + static_cast<uint32_t>(ch - '0');
    }
    return port != 0 && port <= 65535u;
}

bool validHttpsEndpoint(std::string_view endpoint) {
    constexpr std::string_view scheme = "https://";
    constexpr size_t endpoint_capacity = 512;
    constexpr size_t longest_operation = 9; // "heartbeat"
    if (endpoint.size() + 1u + longest_operation + 1u > endpoint_capacity ||
        endpoint.size() <= scheme.size() || endpoint.find_first_of("?#\\") !=
            std::string_view::npos || !isPrintableAscii(endpoint)) {
        return false;
    }
    for (size_t index = 0; index < scheme.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(endpoint[index]);
        const auto rhs = static_cast<unsigned char>(scheme[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    const size_t authority_end = endpoint.find('/', scheme.size());
    const std::string_view authority = endpoint.substr(
        scheme.size(), authority_end == std::string_view::npos
                           ? std::string_view::npos
                           : authority_end - scheme.size());
    if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1) return false;
        const auto suffix = authority.substr(close + 1);
        return suffix.empty() || (suffix.front() == ':' && validPort(suffix.substr(1)));
    }
    const size_t first_colon = authority.find(':');
    const size_t last_colon = authority.rfind(':');
    if (first_colon != last_colon) return false;
    const std::string_view host = authority.substr(0, first_colon);
    if (host.empty()) return false;
    return first_colon == std::string_view::npos ||
        validPort(authority.substr(first_colon + 1));
}


bool validHttpManifestUrl(std::string_view endpoint) {
    constexpr size_t endpoint_capacity = 2048;
    if (endpoint.empty() || endpoint.size() >= endpoint_capacity ||
        endpoint.find_first_of("#\\@") != std::string_view::npos ||
        !isPrintableAscii(endpoint)) {
        return false;
    }

    const size_t scheme_end = endpoint.find("://");
    if (scheme_end == std::string_view::npos) return false;
    const std::string_view scheme = endpoint.substr(0, scheme_end);
    const auto matches = [&](std::string_view expected) {
        if (scheme.size() != expected.size()) return false;
        for (size_t index = 0; index < expected.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(scheme[index])) !=
                static_cast<unsigned char>(expected[index])) {
                return false;
            }
        }
        return true;
    };
    if (!matches("http") && !matches("https")) return false;

    const size_t authority_start = scheme_end + 3u;
    if (authority_start >= endpoint.size()) return false;
    const size_t authority_end = endpoint.find_first_of("/?", authority_start);
    const std::string_view authority = endpoint.substr(
        authority_start,
        authority_end == std::string_view::npos
            ? std::string_view::npos
            : authority_end - authority_start);
    if (authority.empty()) return false;

    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1u) return false;
        const auto suffix = authority.substr(close + 1u);
        if (!suffix.empty() &&
            (suffix.front() != ':' || !validPort(suffix.substr(1)))) {
            return false;
        }
    } else {
        const size_t first_colon = authority.find(':');
        const size_t last_colon = authority.rfind(':');
        if (first_colon != last_colon) return false;
        const std::string_view host = authority.substr(0, first_colon);
        if (host.empty()) return false;
        if (first_colon != std::string_view::npos &&
            !validPort(authority.substr(first_colon + 1))) {
            return false;
        }
    }

    const size_t path_start = endpoint.find('/', authority_start);
    if (path_start == std::string_view::npos) return false;
    const size_t query_start = endpoint.find('?', path_start);
    const std::string_view path = endpoint.substr(
        path_start,
        query_start == std::string_view::npos
            ? std::string_view::npos
            : query_start - path_start);
    return path.size() >= 5u &&
        std::equal(path.end() - 5, path.end(), ".json",
                   [](char lhs, char rhs) {
                       return std::tolower(static_cast<unsigned char>(lhs)) ==
                           std::tolower(static_cast<unsigned char>(rhs));
                   });
}

bool sectionEnabled(const json& section) {
    const auto enabled = section.find("enabled");
    return enabled == section.end() ? true : enabled->get<bool>();
}

fs::path resolvePath(const fs::path& base, const std::string& value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char byte : value) {
        utf8.push_back(static_cast<char8_t>(byte));
    }
    auto path = fs::path(utf8);
    if (path.is_relative()) path = base / path;
    return path.lexically_normal();
}

bool isDirectory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error) && !error;
}

bool isSourceRoot(const fs::path& path) {
    std::error_code error;
    return isDirectory(path / L"python" / L"plugins") &&
        (fs::is_directory(path / L".git", error) ||
         fs::is_regular_file(path / L"C" / L"CMakeLists.txt", error));
}

void appendExistingRoot(const fs::path& candidate,
                        std::vector<std::wstring>& roots) {
    if (!isDirectory(candidate)) return;
    std::error_code error;
    const auto normalized = fs::weakly_canonical(candidate, error);
    const auto value = (error ? candidate.lexically_normal() : normalized).wstring();
    if (std::find(roots.begin(), roots.end(), value) == roots.end()) {
        roots.push_back(value);
    }
}

PluginsProviderConfiguration defaultPluginsConfiguration(
    const fs::path& launcher_base) {
    PluginsProviderConfiguration output;
    if (launcher_base.empty() || launcher_base.is_relative()) return output;

    fs::path source_root;
    for (auto current = launcher_base.lexically_normal(); !current.empty();) {
        if (isSourceRoot(current)) {
            source_root = current;
            break;
        }
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    if (!source_root.empty()) {
        appendExistingRoot(source_root.parent_path() / L"plugins", output.roots);
        appendExistingRoot(source_root / L"python" / L"plugins", output.roots);
    }
    appendExistingRoot(launcher_base / L"plugins", output.roots);
    appendExistingRoot(launcher_base / L"python" / L"plugins", output.roots);
    output.enabled = !output.roots.empty();
    return output;
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
    if (!validHttpsEndpoint(output.endpoint) || output.build_id.empty() ||
        output.build_id.size() > 63u || !isPrintableAscii(output.build_id)) {
        return false;
    }
    if (section->value("responses_prevalidated", false)) return false;
    output.responses_prevalidated = false;
    output.heartbeat_interval_ms = section->value(
        "heartbeat_interval_ms", kDefaultHeartbeatIntervalMs);
    if (output.heartbeat_interval_ms == 0) {
        output.heartbeat_interval_ms = kDefaultHeartbeatIntervalMs;
    }
    const auto public_key = section->find("server_ed25519_pubkey");
    const auto tls_spki_pin = section->find("server_tls_spki_sha256");
    return public_key != section->end() && public_key->is_string() &&
        decodeHex(public_key->get_ref<const std::string&>(),
                  output.server_public_key.data(), output.server_public_key.size()) &&
        !isAllZero(output.server_public_key) &&
        tls_spki_pin != section->end() && tls_spki_pin->is_string() &&
        decodeHex(tls_spki_pin->get_ref<const std::string&>(),
                  output.server_tls_spki_sha256.data(),
                  output.server_tls_spki_sha256.size()) &&
        !isAllZero(output.server_tls_spki_sha256);
}


bool parseUpdate(const json& root, UpdateProviderConfiguration& output) {
    const auto section = root.find("update");
    if (section == root.end()) return true;
    if (!section->is_object()) return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled) return true;

    const auto manifest_url = section->find("manifest_url");
    if (manifest_url == section->end() || !manifest_url->is_string() ||
        manifest_url->get_ref<const std::string&>().empty()) {
        return false;
    }
    output.manifest_url = manifest_url->get<std::string>();
    const auto tls_spki_pin = section->find("server_tls_spki_sha256");
    if (tls_spki_pin == section->end() || !tls_spki_pin->is_string() ||
        !decodeHex(tls_spki_pin->get_ref<const std::string&>(),
                   output.server_tls_spki_sha256.data(),
                   output.server_tls_spki_sha256.size())) {
        return false;
    }
    std::array<uint8_t, 32> native_pin{};
    if (!decodeHex(kNativeUpdateTlsSpkiSha256, native_pin.data(), native_pin.size())) {
        return false;
    }
    return validHttpManifestUrl(output.manifest_url) &&
        output.manifest_url == kNativeUpdateManifestUrl &&
        !isAllZero(output.server_tls_spki_sha256) &&
        equalBytes(output.server_tls_spki_sha256, native_pin);
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
    const auto runtime_manifest = section->find("runtime_manifest");
    if (runtime_manifest != section->end()) {
        if (!runtime_manifest->is_string() ||
            runtime_manifest->get_ref<const std::string&>().empty()) {
            return false;
        }
        output.runtime_manifest_path =
            resolvePath(base, runtime_manifest->get<std::string>()).wstring();
    }
    const auto python_home = section->find("python_home");
    if (python_home != section->end()) {
        if (!python_home->is_string() ||
            python_home->get_ref<const std::string&>().empty()) {
            return false;
        }
        output.python_home =
            resolvePath(base, python_home->get<std::string>()).wstring();
    }
    return !output.roots.empty() || !output.user_roots.empty() ||
        !output.manifests.empty();
}

} // namespace

namespace {

sao_status_t parseProviderConfigurationFile(
    const fs::path& path,
    LauncherProviderConfiguration& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return SAO_STATUS_INVALID_ARGUMENT;
    const auto root = json::parse(input);
    if (!root.is_object() ||
        !parseShell(root, path.parent_path(), output.shell) ||
        !parseLicense(root, output.license) ||
        !parseUpdate(root, output.update) ||
        !parsePlugins(root, path.parent_path(), output.plugins)) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    if (output.shell.enabled) return SAO_STATUS_NOT_IMPLEMENTED;
    return SAO_STATUS_OK;
}

} // namespace

sao_status_t loadLauncherProviderConfiguration(
    const wchar_t* base_dir,
    const wchar_t* config_path) noexcept {
    LauncherProviderConfiguration next;
    if (config_path == nullptr || config_path[0] == L'\0') {
        try {
            const fs::path launcher_base =
                base_dir == nullptr || base_dir[0] == L'\0'
                    ? fs::path{}
                    : fs::path(base_dir).lexically_normal();
            const fs::path default_config = launcher_base.empty()
                ? fs::path{}
                : launcher_base / L"SaoAuto.provider.json";
            std::error_code error;
            if (!default_config.empty() && fs::exists(default_config, error)) {
                if (error) return SAO_STATUS_INVALID_ARGUMENT;
                const auto status =
                    parseProviderConfigurationFile(default_config, next);
                if (status != SAO_STATUS_OK) return status;
            } else {
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
                if (!launcher_base.empty()) {
                    next.plugins = defaultPluginsConfiguration(launcher_base);
                }
#else
                return SAO_STATUS_INVALID_ARGUMENT;
#endif
            }
            std::lock_guard lock(g_configuration_mutex);
            g_configuration = std::move(next);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_INVALID_ARGUMENT;
        }
    }
    try {
        const fs::path launcher_base =
            base_dir == nullptr || base_dir[0] == L'\0'
                ? fs::path{}
                : fs::path(base_dir).lexically_normal();
        fs::path path(config_path);
        if (path.is_relative()) {
            if (launcher_base.empty()) {
                return SAO_STATUS_INVALID_ARGUMENT;
            }
            path = launcher_base / path;
        }
        path = path.lexically_normal();
        const auto status = parseProviderConfigurationFile(path, next);
        if (status != SAO_STATUS_OK) return status;
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