#pragma once

#include "sao/core/status.h"

#include <windows.h>

#include <filesystem>
#include <memory>

namespace sao::launcher::tool_launch::detail {

struct PackagePaths {
    std::filesystem::path executable;
    std::filesystem::path working_directory;
};

class PackageLease final {
public:
    PackageLease() noexcept;
    ~PackageLease();

    PackageLease(PackageLease&&) noexcept;
    PackageLease& operator=(PackageLease&&) noexcept;

    PackageLease(const PackageLease&) = delete;
    PackageLease& operator=(const PackageLease&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    friend sao_status_t create_ai_editor_process(
        const std::filesystem::path&, PROCESS_INFORMATION&, PackagePaths&,
        PackageLease&) noexcept;
    friend sao_status_t acquire_package_lease(
        const std::filesystem::path&, const PackagePaths&,
        PackageLease&) noexcept;
};

sao_status_t resolve_package(const std::filesystem::path& base,
                             PackagePaths& out) noexcept;

sao_status_t acquire_package_lease(const std::filesystem::path& base,
                                   const PackagePaths& package,
                                   PackageLease& out_lease) noexcept;

sao_status_t create_ai_editor_process(const std::filesystem::path& base,
                                      PROCESS_INFORMATION& out_process,
                                      PackagePaths& out_package,
                                      PackageLease& out_lease) noexcept;

} // namespace sao::launcher::tool_launch::detail
