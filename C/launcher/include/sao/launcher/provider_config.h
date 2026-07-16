#pragma once

#include "sao/launcher/init_pipeline.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sao::launcher {

struct ShellProviderConfiguration {
    bool enabled = false;
    std::wstring metadata_path;
};

struct LicenseProviderConfiguration {
    bool enabled = false;
    std::string endpoint;
    std::string build_id;
    std::array<uint8_t, 32> server_public_key{};
    bool responses_prevalidated = false;
    uint32_t heartbeat_interval_ms = 0;
};

struct PluginsProviderConfiguration {
    bool enabled = false;
    std::vector<std::wstring> roots;
    std::vector<std::wstring> user_roots;
    std::vector<std::wstring> manifests;
    bool workspace_walkup = false;
    uint32_t max_depth = 1;
};

struct LauncherProviderConfiguration {
    ShellProviderConfiguration shell;
    LicenseProviderConfiguration license;
    PluginsProviderConfiguration plugins;
};

sao_status_t loadLauncherProviderConfiguration(
    const wchar_t* base_dir,
    const wchar_t* config_path) noexcept;

LauncherProviderConfiguration launcherProviderConfigurationSnapshot();

} // namespace sao::launcher