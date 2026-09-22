// NOTE: must precede sao/launcher/init_pipeline.h (via provider_config.h),
// which #defines SAO_STATUS_OK and would corrupt the canonical enum.
#include "sao/core/crypto.h"

#include "sao/launcher/provider_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace sao::launcher {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr uint32_t kDefaultHeartbeatIntervalMs = 300000U;
constexpr std::string_view kNativeUpdateManifestUrl =
    "https://x2.sjcmc.cn:15018/update/stable/windows-x64-native/latest.json";
constexpr std::string_view kNativeUpdateTlsSpkiSha256 =
    "d3171ec5b86303233b6abda8e83142293d6910099aedd0cdac947d276006db0a";
// License deployment identity — the shipped SaoAuto.provider.json must carry
// exactly this endpoint, TLS SPKI pin and Ed25519 verify key. Rotating any
// of them means a new binary release; a config file alone cannot retarget
// the build at an attacker-controlled licensing service.
constexpr std::string_view kLicenseEndpoint = "https://x2.sjcmc.cn:15522";
constexpr std::string_view kLicenseTlsSpkiSha256 =
    "d3171ec5b86303233b6abda8e83142293d6910099aedd0cdac947d276006db0a";
constexpr std::string_view kLicenseServerEd25519Pubkey =
    "381e5d38fa236a4cd8349e7fdd592e1a4fe83a8f919418fb134da43d174c3bc8";

std::mutex g_configuration_mutex;
LauncherProviderConfiguration g_configuration;

bool decodeHex(std::string_view input, uint8_t* output, size_t output_size) {
    if (input.size() != output_size * 2)
        return false;
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < output_size; ++index) {
        const int high = nibble(input[index * 2]);
        const int low = nibble(input[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool isAllZero(const std::array<uint8_t, 32>& value) {
    uint8_t aggregate = 0;
    for (const uint8_t byte : value)
        aggregate |= byte;
    return aggregate == 0;
}

bool equalBytes(const std::array<uint8_t, 32>& left, const std::array<uint8_t, 32>& right) {
    uint8_t difference = 0;
    for (size_t index = 0; index < left.size(); ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0;
}

bool isPrintableAscii(std::string_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return ch >= 0x20u && ch <= 0x7eu; });
}

bool validPort(std::string_view value) {
    if (value.empty() || value.size() > 5)
        return false;
    uint32_t port = 0;
    for (const unsigned char ch : value) {
        if (!std::isdigit(ch))
            return false;
        port = port * 10u + static_cast<uint32_t>(ch - '0');
    }
    return port != 0 && port <= 65535u;
}

bool validHttpsEndpoint(std::string_view endpoint) {
    constexpr std::string_view scheme = "https://";
    constexpr size_t endpoint_capacity = 512;
    constexpr size_t longest_operation = 9; // "heartbeat"
    if (endpoint.size() + 1u + longest_operation + 1u > endpoint_capacity ||
        endpoint.size() <= scheme.size() ||
        endpoint.find_first_of("?#\\") != std::string_view::npos || !isPrintableAscii(endpoint)) {
        return false;
    }
    for (size_t index = 0; index < scheme.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(endpoint[index]);
        const auto rhs = static_cast<unsigned char>(scheme[index]);
        if (std::tolower(lhs) != std::tolower(rhs))
            return false;
    }
    const size_t authority_end = endpoint.find('/', scheme.size());
    const std::string_view authority = endpoint.substr(
        scheme.size(), authority_end == std::string_view::npos ? std::string_view::npos
                                                               : authority_end - scheme.size());
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1)
            return false;
        const auto suffix = authority.substr(close + 1);
        return suffix.empty() || (suffix.front() == ':' && validPort(suffix.substr(1)));
    }
    const size_t first_colon = authority.find(':');
    const size_t last_colon = authority.rfind(':');
    if (first_colon != last_colon)
        return false;
    const std::string_view host = authority.substr(0, first_colon);
    if (host.empty())
        return false;
    return first_colon == std::string_view::npos || validPort(authority.substr(first_colon + 1));
}

bool validHttpManifestUrl(std::string_view endpoint) {
    constexpr size_t endpoint_capacity = 2048;
    if (endpoint.empty() || endpoint.size() >= endpoint_capacity ||
        endpoint.find_first_of("#\\@") != std::string_view::npos || !isPrintableAscii(endpoint)) {
        return false;
    }

    const size_t scheme_end = endpoint.find("://");
    if (scheme_end == std::string_view::npos)
        return false;
    const std::string_view scheme = endpoint.substr(0, scheme_end);
    const auto matches = [&](std::string_view expected) {
        if (scheme.size() != expected.size())
            return false;
        for (size_t index = 0; index < expected.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(scheme[index])) !=
                static_cast<unsigned char>(expected[index])) {
                return false;
            }
        }
        return true;
    };
    if (!matches("http") && !matches("https"))
        return false;

    const size_t authority_start = scheme_end + 3u;
    if (authority_start >= endpoint.size())
        return false;
    const size_t authority_end = endpoint.find_first_of("/?", authority_start);
    const std::string_view authority = endpoint.substr(
        authority_start, authority_end == std::string_view::npos ? std::string_view::npos
                                                                 : authority_end - authority_start);
    if (authority.empty())
        return false;

    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1u)
            return false;
        const auto suffix = authority.substr(close + 1u);
        if (!suffix.empty() && (suffix.front() != ':' || !validPort(suffix.substr(1)))) {
            return false;
        }
    } else {
        const size_t first_colon = authority.find(':');
        const size_t last_colon = authority.rfind(':');
        if (first_colon != last_colon)
            return false;
        const std::string_view host = authority.substr(0, first_colon);
        if (host.empty())
            return false;
        if (first_colon != std::string_view::npos &&
            !validPort(authority.substr(first_colon + 1))) {
            return false;
        }
    }

    const size_t path_start = endpoint.find('/', authority_start);
    if (path_start == std::string_view::npos)
        return false;
    const size_t query_start = endpoint.find('?', path_start);
    const std::string_view path = endpoint.substr(path_start, query_start == std::string_view::npos
                                                                  ? std::string_view::npos
                                                                  : query_start - path_start);
    return path.size() >= 5u &&
           std::equal(path.end() - 5, path.end(), ".json", [](char lhs, char rhs) {
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
    if (path.is_relative())
        path = base / path;
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

void appendExistingRoot(const fs::path& candidate, std::vector<std::wstring>& roots) {
    if (!isDirectory(candidate))
        return;
    std::error_code error;
    const auto normalized = fs::weakly_canonical(candidate, error);
    const auto value = (error ? candidate.lexically_normal() : normalized).wstring();
    if (std::find(roots.begin(), roots.end(), value) == roots.end()) {
        roots.push_back(value);
    }
}

[[maybe_unused]] PluginsProviderConfiguration
defaultPluginsConfiguration(const fs::path& launcher_base) {
    PluginsProviderConfiguration output;
    if (launcher_base.empty() || launcher_base.is_relative())
        return output;

    fs::path source_root;
    for (auto current = launcher_base.lexically_normal(); !current.empty();) {
        if (isSourceRoot(current)) {
            source_root = current;
            break;
        }
        const auto parent = current.parent_path();
        if (parent == current)
            break;
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

bool parsePathArray(const json& section, const char* name, const fs::path& base,
                    std::vector<std::wstring>& output) {
    const auto values = section.find(name);
    if (values == section.end())
        return true;
    if (!values->is_array())
        return false;
    for (const auto& value : *values) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            return false;
        }
        output.push_back(resolvePath(base, value.get<std::string>()).wstring());
    }
    return true;
}

bool parseShell(const json& root, const fs::path& base, ShellProviderConfiguration& output) {
    const auto section = root.find("shell");
    if (section == root.end())
        return true;
    if (!section->is_object())
        return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled)
        return true;
    const auto metadata = section->find("metadata");
    const auto metadata_path = section->find("metadata_path");
    if (metadata == section->end() && metadata_path == section->end())
        return false;
    const auto valid_path = [&](const json::const_iterator& value) {
        return value != section->end() && value->is_string() &&
               !value->get_ref<const std::string&>().empty();
    };
    if ((metadata != section->end() && !valid_path(metadata)) ||
        (metadata_path != section->end() && !valid_path(metadata_path))) {
        return false;
    }
    const auto selected = metadata_path != section->end() ? metadata_path : metadata;
    const auto resolved = resolvePath(base, selected->get<std::string>());
    if (metadata != section->end() && metadata_path != section->end() &&
        resolvePath(base, metadata->get<std::string>()) != resolved) {
        return false;
    }
    output.metadata_path = resolved.wstring();
    return true;
}

bool parseLicense(const json& root, LicenseProviderConfiguration& output) {
    const auto section = root.find("license");
    if (section == root.end())
        return true;
    if (!section->is_object())
        return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled)
        return true;
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
    if (section->value("responses_prevalidated", false))
        return false;
    output.responses_prevalidated = false;
    output.heartbeat_interval_ms =
        section->value("heartbeat_interval_ms", kDefaultHeartbeatIntervalMs);
    if (output.heartbeat_interval_ms == 0) {
        output.heartbeat_interval_ms = kDefaultHeartbeatIntervalMs;
    }
    const auto public_key = section->find("server_ed25519_pubkey");
    const auto tls_spki_pin = section->find("server_tls_spki_sha256");
    if (public_key == section->end() || !public_key->is_string() ||
        !decodeHex(public_key->get_ref<const std::string&>(), output.server_public_key.data(),
                   output.server_public_key.size()) ||
        tls_spki_pin == section->end() || !tls_spki_pin->is_string() ||
        !decodeHex(tls_spki_pin->get_ref<const std::string&>(),
                   output.server_tls_spki_sha256.data(), output.server_tls_spki_sha256.size())) {
        return false;
    }
    std::array<uint8_t, 32> native_spki{};
    std::array<uint8_t, 32> native_pubkey{};
    if (!decodeHex(kLicenseTlsSpkiSha256, native_spki.data(), native_spki.size()) ||
        !decodeHex(kLicenseServerEd25519Pubkey, native_pubkey.data(), native_pubkey.size())) {
        return false;
    }
    return output.endpoint == kLicenseEndpoint && !isAllZero(output.server_tls_spki_sha256) &&
           equalBytes(output.server_tls_spki_sha256, native_spki) &&
           !isAllZero(output.server_public_key) &&
           equalBytes(output.server_public_key, native_pubkey);
}

bool parseUpdate(const json& root, UpdateProviderConfiguration& output) {
    const auto section = root.find("update");
    if (section == root.end())
        return true;
    if (!section->is_object())
        return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled)
        return true;

    const auto manifest_url = section->find("manifest_url");
    if (manifest_url == section->end() || !manifest_url->is_string() ||
        manifest_url->get_ref<const std::string&>().empty()) {
        return false;
    }
    output.manifest_url = manifest_url->get<std::string>();
    const auto tls_spki_pin = section->find("server_tls_spki_sha256");
    if (tls_spki_pin == section->end() || !tls_spki_pin->is_string() ||
        !decodeHex(tls_spki_pin->get_ref<const std::string&>(),
                   output.server_tls_spki_sha256.data(), output.server_tls_spki_sha256.size())) {
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

bool parsePlugins(const json& root, const fs::path& base, PluginsProviderConfiguration& output) {
    const auto section = root.find("plugins");
    if (section == root.end())
        return true;
    if (!section->is_object())
        return false;
    output.enabled = sectionEnabled(*section);
    if (!output.enabled)
        return true;
    output.workspace_walkup = section->value("workspace_walkup", false);
    output.max_depth = section->value("max_depth", 1U);
    if (output.max_depth == 0)
        return false;
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
        if (!python_home->is_string() || python_home->get_ref<const std::string&>().empty()) {
            return false;
        }
        output.python_home = resolvePath(base, python_home->get<std::string>()).wstring();
    }
    if (!parsePathArray(*section, "python_module_dirs", base, output.python_module_dirs))
        return false;
    const auto dotnet_root = section->find("dotnet_root");
    if (dotnet_root != section->end()) {
        if (!dotnet_root->is_string() || dotnet_root->get_ref<const std::string&>().empty()) {
            return false;
        }
        output.dotnet_root = resolvePath(base, dotnet_root->get<std::string>()).wstring();
    }
    return !output.roots.empty() || !output.user_roots.empty() || !output.manifests.empty();
}

} // namespace

namespace {

// Provider config files sit on disk as an SAO3 envelope ("SAO3" magic,
// AES-256-GCM + DPAPI machine-bound data key, same wire layout as the
// rt_io driver envelope).  A legacy plaintext JSON file is still parsed
// and then transparently re-written as SAO3, so deployment only ever
// ships the plaintext once and every subsequent read hits the
// encrypted form.
constexpr size_t kProviderConfigMaxBytes = 4u * 1024u * 1024u;

bool readProviderConfigBytes(const fs::path& path, std::vector<uint8_t>& output) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > kProviderConfigMaxBytes)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    output.resize(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(output.data()),
                    static_cast<std::streamsize>(output.size()))) {
        return output.empty() && input.eof();
    }
    return true;
}

bool decryptProviderConfig(const std::vector<uint8_t>& envelope,
                           std::vector<uint8_t>& plaintext) {
    size_t plaintext_size = 0;
    sao_status_t status = sao_core_sao3_envelope_decode(
        envelope.data(), envelope.size(), nullptr, 0, &plaintext_size);
    if (status != SAO_STATUS_ERR_BUFFER_TOO_SMALL &&
        status != SAO_STATUS_OK) {
        return false;
    }
    plaintext.resize(plaintext_size);
    status = sao_core_sao3_envelope_decode(
        envelope.data(), envelope.size(), plaintext.data(),
        plaintext.size(), &plaintext_size);
    return status == SAO_STATUS_OK && plaintext_size == plaintext.size();
}

// Best-effort plaintext → SAO3 migration.  Runs AFTER a successful parse
// so a malformed file is never re-encoded.  Writes through a sibling
// temp file + atomic rename so a crash never leaves a truncated config.
void migrateProviderConfigToSao3(const fs::path& path,
                                 const std::vector<uint8_t>& plaintext) {
    size_t envelope_size = 0;
    if (sao_core_sao3_envelope_encode(plaintext.data(), plaintext.size(),
                                      nullptr, 0, &envelope_size) !=
            SAO_STATUS_ERR_BUFFER_TOO_SMALL ||
        envelope_size == 0 || envelope_size > kProviderConfigMaxBytes * 4u) {
        return;
    }
    std::vector<uint8_t> envelope(envelope_size);
    if (sao_core_sao3_envelope_encode(plaintext.data(), plaintext.size(),
                                      envelope.data(), envelope.size(),
                                      &envelope_size) != SAO_STATUS_OK) {
        return;
    }
    envelope.resize(envelope_size);
    const fs::path staging = path.parent_path() / (path.filename().wstring() + L".sao3tmp");
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output ||
            !output.write(reinterpret_cast<const char*>(envelope.data()),
                          static_cast<std::streamsize>(envelope.size()))) {
            std::error_code cleanup;
            fs::remove(staging, cleanup);
            return;
        }
    }
#if defined(_WIN32)
    if (::MoveFileExW(staging.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::error_code cleanup;
        fs::remove(staging, cleanup);
    }
#else
    std::error_code error;
    fs::rename(staging, path, error);
    if (error)
        fs::remove(staging, error);
#endif
}

// The shipped default provider configuration.  When no SaoAuto.provider.json
// exists next to the launcher (fresh install, deleted file, or a file that
// was never staged), the loader writes this blob and parses it.  Identity
// values come from the compile-time constants above so a generated file can
// never smuggle in a foreign endpoint.  The file is written as plaintext;
// parseProviderConfigurationFile transparently migrates it to SAO3 on the
// first successful parse, so every machine re-encrypts under its own
// DPAPI LOCAL_MACHINE key.
std::string defaultProviderConfigJson() {
    json license = json::object();
    license["enabled"] = true;
    license["endpoint"] = kLicenseEndpoint;
    license["build_id"] = "SaoAuto-0.2.0";
    license["server_ed25519_pubkey"] = kLicenseServerEd25519Pubkey;
    license["server_tls_spki_sha256"] = kLicenseTlsSpkiSha256;
    license["responses_prevalidated"] = false;
    license["heartbeat_interval_ms"] = kDefaultHeartbeatIntervalMs;
    json update = json::object();
    update["enabled"] = true;
    update["manifest_url"] = kNativeUpdateManifestUrl;
    update["server_tls_spki_sha256"] = kNativeUpdateTlsSpkiSha256;
    json shell = json::object();
    shell["enabled"] = false;
    json plugins = json::object();
    plugins["enabled"] = true;
    plugins["workspace_walkup"] = false;
    plugins["max_depth"] = 1;
    plugins["roots"] = json::array({"plugins"});
    plugins["user_roots"] = json::array();
    plugins["manifests"] = json::array();
    json root = json::object();
    root["license"] = std::move(license);
    root["update"] = std::move(update);
    root["shell"] = std::move(shell);
    root["plugins"] = std::move(plugins);
    std::string rendered = root.dump(4);
    rendered.push_back('\n');
    return rendered;
}

// Write the generated default config to `path` via a sibling temp file +
// atomic rename.  Returns false on any filesystem failure — callers treat
// that as a hard error because a readable config is mandatory.
bool writeDefaultProviderConfig(const fs::path& path) {
    const std::string rendered = defaultProviderConfigJson();
    const fs::path staging = path.parent_path() / (path.filename().wstring() + L".gentmp");
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(rendered.data(),
                                     static_cast<std::streamsize>(rendered.size()))) {
            std::error_code cleanup;
            fs::remove(staging, cleanup);
            return false;
        }
    }
#if defined(_WIN32)
    if (::MoveFileExW(staging.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::error_code cleanup;
        fs::remove(staging, cleanup);
        return false;
    }
#else
    std::error_code error;
    fs::rename(staging, path, error);
    if (error) {
        fs::remove(staging, error);
        return false;
    }
#endif
    return true;
}

sao_status_t parseProviderConfigurationFile(const fs::path& path,
                                            LauncherProviderConfiguration& output) {
    std::vector<uint8_t> raw;
    if (!readProviderConfigBytes(path, raw))
        return SAO_STATUS_INVALID_ARGUMENT;
    std::vector<uint8_t> plaintext;
    const uint8_t* json_data = raw.data();
    size_t json_size = raw.size();
    const bool encrypted =
        sao_core_sao3_is_envelope(raw.data(), raw.size());
    if (encrypted) {
        if (!decryptProviderConfig(raw, plaintext))
            return SAO_STATUS_INVALID_ARGUMENT;
        json_data = plaintext.data();
        json_size = plaintext.size();
    }
    const auto root = json::parse(json_data, json_data + json_size);
    if (!root.is_object() || !parseShell(root, path.parent_path(), output.shell) ||
        !parseLicense(root, output.license) || !parseUpdate(root, output.update) ||
        !parsePlugins(root, path.parent_path(), output.plugins)) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    if (!encrypted)
        migrateProviderConfigToSao3(path, raw);
    return SAO_STATUS_OK;
}

} // namespace

sao_status_t loadLauncherProviderConfiguration(const wchar_t* base_dir,
                                               const wchar_t* config_path) noexcept {
    LauncherProviderConfiguration next;
    if (config_path == nullptr || config_path[0] == L'\0') {
        try {
            const fs::path launcher_base = base_dir == nullptr || base_dir[0] == L'\0'
                                               ? fs::path{}
                                               : fs::path(base_dir).lexically_normal();
            const fs::path default_config =
                launcher_base.empty() ? fs::path{} : launcher_base / L"SaoAuto.provider.json";
            std::error_code error;
            bool have_config =
                !default_config.empty() && fs::exists(default_config, error);
            if (error)
                return SAO_STATUS_INVALID_ARGUMENT;
            if (have_config) {
                // A machine-bound SAO3 envelope produced on another machine
                // will not unwrap here; treat that exactly like a missing
                // file and regenerate the shipped defaults.
                std::vector<uint8_t> probe;
                if (readProviderConfigBytes(default_config, probe) &&
                    sao_core_sao3_is_envelope(probe.data(), probe.size())) {
                    std::vector<uint8_t> plaintext;
                    if (!decryptProviderConfig(probe, plaintext))
                        have_config = false;
                }
            }
            if (have_config) {
                const auto status = parseProviderConfigurationFile(default_config, next);
                if (status != SAO_STATUS_OK)
                    return status;
            } else {
                if (default_config.empty())
                    return SAO_STATUS_INVALID_ARGUMENT;
                // The config is deployment data, not a protected artifact:
                // it must be self-healing.  Generate the shipped defaults and
                // parse them; the plaintext file is upgraded to SAO3 by the
                // parse path on first success.
                if (!writeDefaultProviderConfig(default_config))
                    return SAO_STATUS_INVALID_ARGUMENT;
                const auto status = parseProviderConfigurationFile(default_config, next);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            std::lock_guard lock(g_configuration_mutex);
            g_configuration = std::move(next);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_INVALID_ARGUMENT;
        }
    }
    try {
        const fs::path launcher_base = base_dir == nullptr || base_dir[0] == L'\0'
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
        if (status != SAO_STATUS_OK)
            return status;
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