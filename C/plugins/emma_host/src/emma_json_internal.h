#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "sao/plugins/emma_host/emma_interpreter.h"

namespace sao::plugins::emma_host::detail {

using json = nlohmann::json;

inline constexpr std::size_t kMaximumEmmaJsonInputBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaximumEmmaJsonDepth = 64;
inline constexpr std::size_t kMaximumEmmaJsonNodes = 16384;
inline constexpr std::size_t kMaximumEmmaJsonStringBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumEmmaJsonTotalStringBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumEmmaJsonOutputBytes = 8U * 1024U * 1024U;

struct json_budget final {
    std::size_t nodes = 0;
    std::size_t string_bytes = 0;
};

inline bool valid_utf8(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = bytes[index];
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            count = 2;
            codepoint = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            count = 3;
            codepoint = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            count = 4;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (count > value.size() - index)
            return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((count == 3 && codepoint < 0x800U) ||
            (count == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
            return false;
        }
        index += count;
    }
    return true;
}

inline bool consume_json_node(json_budget& budget, std::string& error) {
    if (budget.nodes >= kMaximumEmmaJsonNodes) {
        error = "JSON value exceeds its node budget";
        return false;
    }
    ++budget.nodes;
    return true;
}

inline bool consume_json_string(std::string_view value, json_budget& budget, std::string& error) {
    if (value.find('\0') != std::string_view::npos || !valid_utf8(value)) {
        error = "JSON strings must be valid UTF-8";
        return false;
    }
    if (value.size() > kMaximumEmmaJsonStringBytes ||
        budget.string_bytes > kMaximumEmmaJsonTotalStringBytes - value.size()) {
        error = "JSON value exceeds its string budget";
        return false;
    }
    budget.string_bytes += value.size();
    return true;
}

class bounded_json_sax final : public nlohmann::json_sax<json> {
  public:
    explicit bounded_json_sax(std::string& error) noexcept : error_(error) {}

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
        if (value > static_cast<number_unsigned_t>((std::numeric_limits<std::int64_t>::max)())) {
            error_ = "JSON unsigned integer exceeds Emma's signed integer range";
            return false;
        }
        return consume_node();
    }
    bool number_float(number_float_t, const string_t&) override {
        return consume_node();
    }
    bool string(string_t& value) override {
        return consume_node() && consume_string(value);
    }
    bool binary(binary_t&) override {
        error_ = "binary JSON values are unsupported";
        return false;
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
        if (error_.empty())
            error_ = "JSON input is invalid";
        return false;
    }

  private:
    bool consume_node() {
        return consume_json_node(budget_, error_);
    }

    bool consume_string(std::string_view value) {
        return consume_json_string(value, budget_, error_);
    }

    bool start_container() {
        if (depth_ >= kMaximumEmmaJsonDepth) {
            error_ = "JSON value nesting exceeds 64 levels";
            return false;
        }
        if (!consume_node())
            return false;
        ++depth_;
        return true;
    }

    bool end_container() {
        if (depth_ == 0) {
            error_ = "JSON input is invalid";
            return false;
        }
        --depth_;
        return true;
    }

    std::string& error_;
    json_budget budget_;
    std::size_t depth_ = 0;
};

inline bool parse_json(const char* data, std::size_t size, json& output, std::string& error) {
    error.clear();
    if (data == nullptr || size > kMaximumEmmaJsonInputBytes ||
        !valid_utf8(std::string_view(data == nullptr ? "" : data, data == nullptr ? 0 : size))) {
        error = "JSON input is invalid or exceeds its byte budget";
        return false;
    }
    try {
        bounded_json_sax sax(error);
        if (!json::sax_parse(data, data + size, &sax)) {
            if (error.empty())
                error = "JSON input is invalid";
            return false;
        }
        json candidate = json::parse(data, data + size, nullptr, false);
        if (candidate.is_discarded()) {
            error = "JSON input is invalid";
            return false;
        }
        output = std::move(candidate);
        return true;
    } catch (...) {
        error = "JSON input conversion failed";
        return false;
    }
}

inline bool bounded_json_c_string(const char* data, std::size_t maximum,
                                  std::size_t& length) noexcept {
    if (data == nullptr)
        return false;
    length = 0;
    while (length <= maximum && data[length] != '\0')
        ++length;
    return length <= maximum;
}

inline bool parse_json_c_string(const char* data, json& output, std::string& error) {
    std::size_t size = 0;
    if (!bounded_json_c_string(data, kMaximumEmmaJsonInputBytes, size)) {
        error = "JSON input exceeds its byte budget";
        return false;
    }
    return parse_json(data, size, output, error);
}

inline bool json_to_emma_value(const json& value, emma_value& output, json_budget& budget,
                               std::string& error, std::size_t depth) {
    const bool container = value.is_array() || value.is_object();
    if (container && depth >= kMaximumEmmaJsonDepth) {
        error = "JSON value nesting exceeds 64 levels";
        return false;
    }
    if (!consume_json_node(budget, error))
        return false;

    if (value.is_null()) {
        output = nullptr;
        return true;
    }
    if (value.is_boolean()) {
        output = value.get<bool>();
        return true;
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            error = "JSON unsigned integer exceeds Emma's signed integer range";
            return false;
        }
        output = static_cast<std::int64_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        output = value.get<std::int64_t>();
        return true;
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            error = "JSON numbers must be finite";
            return false;
        }
        output = number;
        return true;
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (!consume_json_string(text, budget, error))
            return false;
        output = text;
        return true;
    }
    if (value.is_array()) {
        auto candidate = std::make_shared<emma_list>();
        candidate->items.reserve(value.size());
        for (const auto& item : value) {
            emma_value converted = nullptr;
            if (!json_to_emma_value(item, converted, budget, error, depth + 1))
                return false;
            candidate->items.push_back(std::move(converted));
        }
        output = std::move(candidate);
        return true;
    }
    if (value.is_object()) {
        auto candidate = std::make_shared<emma_dict>();
        candidate->items.reserve(value.size());
        for (const auto& [key, item] : value.items()) {
            if (!consume_json_string(key, budget, error))
                return false;
            emma_value converted = nullptr;
            if (!json_to_emma_value(item, converted, budget, error, depth + 1))
                return false;
            candidate->items.emplace(key, std::move(converted));
        }
        output = std::move(candidate);
        return true;
    }
    error = "JSON value is not representable in Emma";
    return false;
}

inline bool json_to_emma_value(const json& value, emma_value& output, std::string& error) {
    try {
        json_budget budget;
        emma_value candidate = nullptr;
        if (!json_to_emma_value(value, candidate, budget, error, 0))
            return false;
        output = std::move(candidate);
        return true;
    } catch (...) {
        error = "Emma JSON conversion failed";
        return false;
    }
}

inline bool parse_json_to_emma_value(const char* data, std::size_t size, emma_value& output,
                                     std::string& error) {
    json parsed;
    if (!parse_json(data, size, parsed, error))
        return false;
    return json_to_emma_value(parsed, output, error);
}

inline bool parse_json_c_string_to_emma_value(const char* data, emma_value& output,
                                              std::string& error) {
    json parsed;
    if (!parse_json_c_string(data, parsed, error))
        return false;
    return json_to_emma_value(parsed, output, error);
}

inline bool emma_value_to_json(const emma_value& value, json& output, json_budget& budget,
                               std::string& error, std::size_t depth) {
    const auto* list = std::get_if<std::shared_ptr<emma_list>>(&value);
    const auto* dictionary = std::get_if<std::shared_ptr<emma_dict>>(&value);
    const bool container = (list != nullptr && *list != nullptr) ||
                           (dictionary != nullptr && *dictionary != nullptr);
    if (container && depth >= kMaximumEmmaJsonDepth) {
        error = "Emma value nesting exceeds 64 levels";
        return false;
    }
    if (!consume_json_node(budget, error))
        return false;

    if (std::holds_alternative<std::nullptr_t>(value)) {
        output = nullptr;
        return true;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        output = *boolean;
        return true;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        output = *integer;
        return true;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number)) {
            error = "Emma JSON numbers must be finite";
            return false;
        }
        output = *number;
        return true;
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (!consume_json_string(*text, budget, error))
            return false;
        output = *text;
        return true;
    }
    if (list != nullptr) {
        if (*list == nullptr) {
            output = nullptr;
            return true;
        }
        json candidate = json::array();
        for (const auto& item : (*list)->items) {
            json converted;
            if (!emma_value_to_json(item, converted, budget, error, depth + 1))
                return false;
            candidate.push_back(std::move(converted));
        }
        output = std::move(candidate);
        return true;
    }
    if (dictionary != nullptr) {
        if (*dictionary == nullptr) {
            output = nullptr;
            return true;
        }
        json candidate = json::object();
        for (const auto& [key, item] : (*dictionary)->items) {
            if (!consume_json_string(key, budget, error))
                return false;
            json converted;
            if (!emma_value_to_json(item, converted, budget, error, depth + 1))
                return false;
            candidate[key] = std::move(converted);
        }
        output = std::move(candidate);
        return true;
    }
    error = "Emma callable values are not JSON serializable";
    return false;
}

inline bool emma_value_to_json(const emma_value& value, json& output, std::string& error) {
    try {
        json_budget budget;
        json candidate;
        if (!emma_value_to_json(value, candidate, budget, error, 0))
            return false;
        output = std::move(candidate);
        return true;
    } catch (...) {
        error = "Emma JSON conversion failed";
        return false;
    }
}

inline bool serialize_json(const json& value, std::string& output, std::string& error) {
    try {
        std::string candidate = value.dump(-1, ' ', false, json::error_handler_t::strict);
        if (candidate.size() > kMaximumEmmaJsonOutputBytes) {
            error = "JSON output exceeds its byte budget";
            return false;
        }
        output = std::move(candidate);
        return true;
    } catch (...) {
        error = "JSON output is not valid UTF-8";
        return false;
    }
}

inline bool serialize_emma_value(const emma_value& value, std::string& output,
                                 std::string& error) {
    json converted;
    if (!emma_value_to_json(value, converted, error))
        return false;
    return serialize_json(converted, output, error);
}

inline bool validate_json(std::string_view value, std::string& error) {
    json parsed;
    return parse_json(value.data(), value.size(), parsed, error);
}

} // namespace sao::plugins::emma_host::detail
