#include "sao/net/url.h"

#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

constexpr size_t kMaximumUrlBytes = 2048;
constexpr size_t kMaximumHostBytes = 253;

bool is_scheme_first(unsigned char value) noexcept {
    return std::isalpha(value) != 0;
}

bool is_scheme_character(unsigned char value) noexcept {
    return std::isalnum(value) != 0 || value == '+' || value == '-' || value == '.';
}

bool is_unreserved(unsigned char value) noexcept {
    return std::isalnum(value) != 0 || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

int decode_nibble(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool valid_utf8(std::string_view value) noexcept {
    size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7f) {
            ++offset;
            continue;
        }
        size_t continuation_count = 0;
        uint32_t code_point = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1;
            code_point = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2;
            code_point = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3;
            code_point = first & 0x07u;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size()) return false;
        for (size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0u) != 0x80u) return false;
            code_point = (code_point << 6u) | (next & 0x3fu);
        }
        const bool overlong =
            (continuation_count == 1 && code_point < 0x80u) ||
            (continuation_count == 2 && code_point < 0x800u) ||
            (continuation_count == 3 && code_point < 0x10000u);
        if (overlong || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

bool assign_view(uint16_t& offset_out, uint16_t& length_out,
                 size_t offset, size_t length) noexcept {
    if (offset > std::numeric_limits<uint16_t>::max() ||
        length > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    offset_out = static_cast<uint16_t>(offset);
    length_out = static_cast<uint16_t>(length);
    return true;
}

sao_status_t write_component(std::string_view input, bool decode,
                             char* output, size_t capacity,
                             size_t* bytes_needed) noexcept {
    if (bytes_needed == nullptr || (output == nullptr && capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *bytes_needed = 0;
    if (output != nullptr && capacity != 0) output[0] = '\0';
    if (!valid_utf8(input)) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    size_t required = 1;
    if (decode) {
        for (size_t index = 0; index < input.size(); ++index) {
            if (input[index] == '%') {
                if (index + 2 >= input.size() || decode_nibble(input[index + 1]) < 0 ||
                    decode_nibble(input[index + 2]) < 0) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                index += 2;
            }
            ++required;
        }
    } else {
        for (const auto value : input) {
            required += is_unreserved(static_cast<unsigned char>(value)) ? 1 : 3;
        }
    }
    *bytes_needed = required;
    if (output == nullptr) return SAO_STATUS_OK;
    if (capacity < required) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;

    static constexpr std::array<char, 16> kHex = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    size_t written = 0;
    for (size_t index = 0; index < input.size(); ++index) {
        const auto value = static_cast<unsigned char>(input[index]);
        if (decode && value == '%') {
            const auto high = decode_nibble(input[index + 1]);
            const auto low = decode_nibble(input[index + 2]);
            const auto decoded = static_cast<unsigned char>((high << 4) | low);
            if (decoded == 0) {
                output[0] = '\0';
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            output[written++] = static_cast<char>(decoded);
            index += 2;
        } else if (!decode && !is_unreserved(value)) {
            output[written++] = '%';
            output[written++] = kHex[value >> 4u];
            output[written++] = kHex[value & 0x0fu];
        } else {
            output[written++] = static_cast<char>(value);
        }
    }
    output[written] = '\0';
    if (decode && !valid_utf8(std::string_view(output, written))) {
        output[0] = '\0';
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_url_parse(
    const char* url_utf8, SaoUrlParts* out_parts) {
    if (out_parts == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(out_parts, 0, sizeof(*out_parts));
    if (url_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    const size_t length = ::strnlen(url_utf8, kMaximumUrlBytes + 1);
    if (length == 0 || length > kMaximumUrlBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string_view url(url_utf8, length);
    if (!valid_utf8(url)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    for (const auto value : url) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte <= 0x20u || byte == 0x7fu) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const size_t scheme_end = url.find(':');
    if (scheme_end == std::string_view::npos || scheme_end == 0 ||
        !is_scheme_first(static_cast<unsigned char>(url[0]))) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (size_t index = 1; index < scheme_end; ++index) {
        if (!is_scheme_character(static_cast<unsigned char>(url[index]))) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    if (scheme_end + 2 >= url.size() || url[scheme_end + 1] != '/' ||
        url[scheme_end + 2] != '/') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const size_t authority_begin = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_begin);
    const size_t authority_limit = authority_end == std::string_view::npos
                                       ? url.size()
                                       : authority_end;
    if (authority_begin == authority_limit ||
        url.substr(authority_begin, authority_limit - authority_begin).find('@') !=
            std::string_view::npos) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    size_t host_begin = authority_begin;
    size_t host_end = authority_limit;
    size_t port_begin = std::string_view::npos;
    if (url[host_begin] == '[') {
        const size_t closing = url.find(']', host_begin + 1);
        if (closing == std::string_view::npos || closing >= authority_limit ||
            closing == host_begin + 1) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        host_begin += 1;
        host_end = closing;
        if (closing + 1 < authority_limit) {
            if (url[closing + 1] != ':') return SAO_STATUS_ERR_INVALID_ARGUMENT;
            port_begin = closing + 2;
        }
    } else {
        const size_t colon = url.find(':', host_begin);
        if (colon != std::string_view::npos && colon < authority_limit) {
            host_end = colon;
            port_begin = colon + 1;
        }
    }
    const size_t host_length = host_end - host_begin;
    if (host_length == 0 || host_length > kMaximumHostBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    uint16_t port = 0;
    if (port_begin != std::string_view::npos) {
        if (port_begin >= authority_limit) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        uint32_t parsed = 0;
        for (size_t index = port_begin; index < authority_limit; ++index) {
            const auto value = static_cast<unsigned char>(url[index]);
            if (std::isdigit(value) == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            parsed = parsed * 10u + static_cast<uint32_t>(value - '0');
            if (parsed > 65535u) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (parsed == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        port = static_cast<uint16_t>(parsed);
    }

    const size_t fragment_marker = url.find('#', authority_limit);
    const size_t query_marker = url.find('?', authority_limit);
    if (query_marker != std::string_view::npos && fragment_marker != std::string_view::npos &&
        query_marker > fragment_marker) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const size_t path_end = std::min(
        query_marker == std::string_view::npos ? url.size() : query_marker,
        fragment_marker == std::string_view::npos ? url.size() : fragment_marker);
    const size_t path_begin = authority_limit;
    const size_t path_length = path_end - path_begin;
    if (path_length != 0 && url[path_begin] != '/') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (!assign_view(out_parts->scheme_off, out_parts->scheme_len, 0, scheme_end) ||
        !assign_view(out_parts->host_off, out_parts->host_len, host_begin, host_length) ||
        !assign_view(out_parts->path_off, out_parts->path_len, path_begin, path_length)) {
        std::memset(out_parts, 0, sizeof(*out_parts));
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (query_marker != std::string_view::npos) {
        const size_t begin = query_marker + 1;
        const size_t end = fragment_marker == std::string_view::npos
                               ? url.size()
                               : fragment_marker;
        if (!assign_view(out_parts->query_off, out_parts->query_len, begin, end - begin)) {
            std::memset(out_parts, 0, sizeof(*out_parts));
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    if (fragment_marker != std::string_view::npos) {
        const size_t begin = fragment_marker + 1;
        if (!assign_view(out_parts->fragment_off, out_parts->fragment_len,
                         begin, url.size() - begin)) {
            std::memset(out_parts, 0, sizeof(*out_parts));
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    out_parts->port = port;
    out_parts->port_present = port_begin != std::string_view::npos;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_url_encode_component(
    const char* input_utf8, char* out_utf8, size_t capacity,
    size_t* out_bytes_needed) {
    if (input_utf8 == nullptr) {
        if (out_bytes_needed != nullptr) *out_bytes_needed = 0;
        if (out_utf8 != nullptr && capacity != 0) out_utf8[0] = '\0';
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return write_component(input_utf8, false, out_utf8, capacity, out_bytes_needed);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_url_decode_component(
    const char* input_utf8, char* out_utf8, size_t capacity,
    size_t* out_bytes_needed) {
    if (input_utf8 == nullptr) {
        if (out_bytes_needed != nullptr) *out_bytes_needed = 0;
        if (out_utf8 != nullptr && capacity != 0) out_utf8[0] = '\0';
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return write_component(input_utf8, true, out_utf8, capacity, out_bytes_needed);
}
