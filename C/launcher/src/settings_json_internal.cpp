#include "settings_json_internal.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace sao::launcher::settings_json {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t kMaxNestingDepth = 128;

struct NestingDepthExceeded final {};

void secure_zero(std::string& value) noexcept {
    if (!value.empty()) {
        ::SecureZeroMemory(value.data(), value.size());
    }
}

class SensitiveString final {
  public:
    SensitiveString() = default;
    ~SensitiveString() noexcept {
        secure_zero(value_);
    }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;

    std::string& get() noexcept {
        return value_;
    }

  private:
    std::string value_;
};

bool add_serialized_size(std::size_t amount, std::size_t max_output_bytes,
                         std::size_t& total) noexcept {
    if (amount > (std::numeric_limits<std::size_t>::max)() - total) {
        return false;
    }
    const std::size_t updated = total + amount;
    if (updated > max_output_bytes) {
        return false;
    }
    total = updated;
    return true;
}

std::size_t utf8_sequence_size(std::string_view value, std::size_t offset) noexcept {
    const auto first = static_cast<unsigned char>(value[offset]);
    const auto continuation = [&value](std::size_t index) noexcept {
        return index < value.size() && (static_cast<unsigned char>(value[index]) & 0xc0U) == 0x80U;
    };

    if (first <= 0x7fU)
        return 1;
    if (first >= 0xc2U && first <= 0xdfU) {
        return continuation(offset + 1) ? 2 : 0;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        if (!continuation(offset + 1) || !continuation(offset + 2))
            return 0;
        const auto second = static_cast<unsigned char>(value[offset + 1]);
        if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second > 0x9fU)) {
            return 0;
        }
        return 3;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        if (!continuation(offset + 1) || !continuation(offset + 2) || !continuation(offset + 3)) {
            return 0;
        }
        const auto second = static_cast<unsigned char>(value[offset + 1]);
        if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU)) {
            return 0;
        }
        return 4;
    }
    return 0;
}

sao_status_t measure_string(std::string_view value, std::size_t max_output_bytes,
                            std::size_t& total) noexcept {
    if (!add_serialized_size(2, max_output_bytes, total)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte >= 0x80U) {
            const std::size_t sequence_size = utf8_sequence_size(value, index);
            if (sequence_size == 0 ||
                !add_serialized_size(sequence_size, max_output_bytes, total)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            index += sequence_size;
            continue;
        }

        const std::size_t escaped_size = byte == '"' || byte == '\\' || byte == '\b' ||
                                                 byte == '\f' || byte == '\n' || byte == '\r' ||
                                                 byte == '\t'
                                             ? 2U
                                         : byte < 0x20U ? 6U
                                                        : 1U;
        if (!add_serialized_size(escaped_size, max_output_bytes, total)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        ++index;
    }
    return SAO_STATUS_OK;
}

sao_status_t number_text(const Json& value, std::string& out) {
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out = value.dump(-1, ' ', false, Json::error_handler_t::strict);
    return SAO_STATUS_OK;
}

sao_status_t measure(const Json& value, std::size_t max_output_bytes, std::size_t depth,
                     std::size_t& total) {
    if (depth > kMaxNestingDepth) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (value.is_null()) {
        return add_serialized_size(4, max_output_bytes, total) ? SAO_STATUS_OK
                                                               : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (value.is_boolean()) {
        return add_serialized_size(value.get<bool>() ? 4U : 5U, max_output_bytes, total)
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (value.is_number()) {
        std::string number;
        const auto status = number_text(value, number);
        if (status != SAO_STATUS_OK)
            return status;
        return add_serialized_size(number.size(), max_output_bytes, total)
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (value.is_string()) {
        return measure_string(value.get_ref<const std::string&>(), max_output_bytes, total);
    }
    if (value.is_array()) {
        if (!add_serialized_size(2, max_output_bytes, total)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        bool first = true;
        for (const auto& item : value) {
            if (!first && !add_serialized_size(2, max_output_bytes, total)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            first = false;
            const auto status = measure(item, max_output_bytes, depth + 1, total);
            if (status != SAO_STATUS_OK)
                return status;
        }
        return SAO_STATUS_OK;
    }
    if (value.is_object()) {
        if (!add_serialized_size(2, max_output_bytes, total)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        bool first = true;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (!first && !add_serialized_size(2, max_output_bytes, total)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            first = false;
            auto status = measure_string(item.key(), max_output_bytes, total);
            if (status != SAO_STATUS_OK)
                return status;
            if (!add_serialized_size(2, max_output_bytes, total)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            status = measure(item.value(), max_output_bytes, depth + 1, total);
            if (status != SAO_STATUS_OK)
                return status;
        }
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

sao_status_t append_string(std::string_view value, std::string& out) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte >= 0x80U) {
            const std::size_t sequence_size = utf8_sequence_size(value, index);
            if (sequence_size == 0) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            out.append(value.substr(index, sequence_size));
            index += sequence_size;
            continue;
        }

        switch (byte) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                out.append("\\u00");
                out.push_back(kHexDigits[byte >> 4U]);
                out.push_back(kHexDigits[byte & 0x0fU]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
        ++index;
    }
    out.push_back('"');
    return SAO_STATUS_OK;
}

sao_status_t append(const Json& value, std::size_t depth, std::string& out) {
    if (depth > kMaxNestingDepth) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (value.is_null()) {
        out.append("null");
        return SAO_STATUS_OK;
    }
    if (value.is_boolean()) {
        out.append(value.get<bool>() ? "true" : "false");
        return SAO_STATUS_OK;
    }
    if (value.is_number()) {
        std::string number;
        const auto status = number_text(value, number);
        if (status != SAO_STATUS_OK)
            return status;
        out.append(number);
        return SAO_STATUS_OK;
    }
    if (value.is_string()) {
        return append_string(value.get_ref<const std::string&>(), out);
    }
    if (value.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : value) {
            if (!first)
                out.append(", ");
            first = false;
            const auto status = append(item, depth + 1, out);
            if (status != SAO_STATUS_OK)
                return status;
        }
        out.push_back(']');
        return SAO_STATUS_OK;
    }
    if (value.is_object()) {
        out.push_back('{');
        bool first = true;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (!first)
                out.append(", ");
            first = false;
            auto status = append_string(item.key(), out);
            if (status != SAO_STATUS_OK)
                return status;
            out.append(": ");
            status = append(item.value(), depth + 1, out);
            if (status != SAO_STATUS_OK)
                return status;
        }
        out.push_back('}');
        return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

} // namespace

sao_status_t parse_strict_limited(std::string_view input, Json& out) noexcept {
    try {
        const auto depth_callback = [](int depth, Json::parse_event_t event, Json&) {
            if ((event == Json::parse_event_t::object_start ||
                 event == Json::parse_event_t::array_start) &&
                (depth < 0 || static_cast<std::size_t>(depth) > kMaxNestingDepth)) {
                throw NestingDepthExceeded{};
            }
            return true;
        };

        Json parsed = Json::parse(input.begin(), input.end(), depth_callback, true, false);
        out.swap(parsed);
        return SAO_STATUS_OK;
    } catch (const NestingDepthExceeded&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t serialize_python_compatible(const Json& document, std::size_t max_output_bytes,
                                         std::string& out) noexcept {
    try {
        std::size_t serialized_size = 0;
        auto status = measure(document, max_output_bytes, 0, serialized_size);
        if (status != SAO_STATUS_OK)
            return status;

        SensitiveString serialized_storage;
        auto& serialized = serialized_storage.get();
        serialized.reserve(serialized_size);
        status = append(document, 0, serialized);
        if (status != SAO_STATUS_OK)
            return status;
        if (serialized.size() != serialized_size) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        out = std::move(serialized);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::settings_json
