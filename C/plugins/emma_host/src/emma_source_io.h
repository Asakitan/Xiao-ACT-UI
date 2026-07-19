#pragma once

#include "sao_plugins/sao_status.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace sao::plugins::emma_host {

inline constexpr size_t kMaximumEmmaSourceBytes = 8u * 1024u * 1024u;

int32_t read_emma_source_file(const std::filesystem::path& plugin_root,
                              std::string_view relative_path, std::string& out_source,
                              std::filesystem::path& out_final_path,
                              std::string& out_error_message) noexcept;

} // namespace sao::plugins::emma_host
