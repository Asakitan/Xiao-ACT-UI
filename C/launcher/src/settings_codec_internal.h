#pragma once

#include "sao/core/status.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace sao::launcher::settings_codec {

using Json = nlohmann::ordered_json;

struct DecodeResult {
    Json document;
    bool legacy_plaintext = false;
};

sao_status_t encode(const Json& document, std::string& out_envelope_utf8) noexcept;

sao_status_t decode(std::string_view raw_utf8, DecodeResult& out) noexcept;

} // namespace sao::launcher::settings_codec
