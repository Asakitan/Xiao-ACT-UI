#pragma once

#include "sao/core/status.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace sao::launcher::settings_json {

sao_status_t parse_strict_limited(std::string_view input, nlohmann::ordered_json& out) noexcept;

sao_status_t serialize_python_compatible(const nlohmann::ordered_json& document,
                                         std::size_t max_output_bytes, std::string& out) noexcept;

} // namespace sao::launcher::settings_json
