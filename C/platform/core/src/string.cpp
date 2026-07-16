#include "sao/core/string.h"

#include <cstring>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<unsigned char>(value + ('a' - 'A'))
        : value;
}

bool is_ascii_whitespace(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_string_utf8_to_utf16(
    const char* utf8,
    wchar_t* out_wide,
    size_t wide_capacity,
    size_t* out_wide_count) {
#ifdef _WIN32
    if (utf8 == nullptr || out_wide_count == nullptr ||
        (out_wide == nullptr && wide_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_wide_count = 0;
    if (std::strlen(utf8) > static_cast<size_t>(std::numeric_limits<int>::max() - 1)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (required <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_wide_count = static_cast<size_t>(required);
    if (out_wide == nullptr) {
        return SAO_STATUS_OK;
    }
    if (wide_capacity < static_cast<size_t>(required)) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out_wide, required) == required
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)utf8;
    (void)out_wide;
    (void)wide_capacity;
    if (out_wide_count != nullptr) {
        *out_wide_count = 0;
    }
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_string_utf16_to_utf8(
    const wchar_t* utf16,
    char* out_utf8,
    size_t utf8_capacity,
    size_t* out_utf8_count) {
#ifdef _WIN32
    if (utf16 == nullptr || out_utf8_count == nullptr ||
        (out_utf8 == nullptr && utf8_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_utf8_count = 0;
    if (std::wcslen(utf16) > static_cast<size_t>(std::numeric_limits<int>::max() - 1)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_utf8_count = static_cast<size_t>(required);
    if (out_utf8 == nullptr) {
        return SAO_STATUS_OK;
    }
    if (utf8_capacity < static_cast<size_t>(required)) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return WideCharToMultiByte(CP_UTF8,
                               WC_ERR_INVALID_CHARS,
                               utf16,
                               -1,
                               out_utf8,
                               required,
                               nullptr,
                               nullptr) == required
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)utf16;
    (void)out_utf8;
    (void)utf8_capacity;
    if (out_utf8_count != nullptr) {
        *out_utf8_count = 0;
    }
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" int32_t SAO_CORE_CALL sao_core_string_ascii_icmp(
    const char* a, const char* b) {
    if (a == b) {
        return 0;
    }
    if (a == nullptr) {
        return -1;
    }
    if (b == nullptr) {
        return 1;
    }
    while (*a != '\0' && *b != '\0') {
        const auto left = ascii_lower(static_cast<unsigned char>(*a));
        const auto right = ascii_lower(static_cast<unsigned char>(*b));
        if (left != right) {
            return left < right ? -1 : 1;
        }
        ++a;
        ++b;
    }
    const auto left = static_cast<unsigned char>(*a);
    const auto right = static_cast<unsigned char>(*b);
    return left == right ? 0 : (left < right ? -1 : 1);
}

extern "C" size_t SAO_CORE_CALL sao_core_string_ascii_trim(char* buffer) {
    if (buffer == nullptr) {
        return 0;
    }
    const size_t original_length = std::strlen(buffer);
    size_t first = 0;
    while (first < original_length &&
           is_ascii_whitespace(static_cast<unsigned char>(buffer[first]))) {
        ++first;
    }
    size_t end = original_length;
    while (end > first &&
           is_ascii_whitespace(static_cast<unsigned char>(buffer[end - 1]))) {
        --end;
    }
    const size_t trailing_removed = original_length - end;
    const size_t trimmed_length = end - first;
    if (first != 0 && trimmed_length != 0) {
        std::memmove(buffer, buffer + first, trimmed_length);
    }
    buffer[trimmed_length] = '\0';
    return trailing_removed;
}

extern "C" uint64_t SAO_CORE_CALL sao_core_string_fnv1a64(
    const void* data, size_t length) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
