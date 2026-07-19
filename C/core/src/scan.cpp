#include "sao_core/scan.h"

#include "logging.h"

#include <cstring>
#include <limits>

namespace {

constexpr char kComponent[] = "core.scan";

int32_t fail(int32_t status, const char* message) noexcept {
    sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, kComponent, status, message);
    return status;
}

bool is_target_value(uint64_t value, const uint64_t* targets, size_t target_count) noexcept {
    for (size_t index = 0; index < target_count; ++index) {
        if (targets[index] == value) {
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_scan_find_pattern(
    const uint8_t* haystack, size_t haystack_len, const uint8_t* pattern, const uint8_t* mask,
    size_t pattern_len, size_t* out_offset) {
    if (out_offset != nullptr) {
        *out_offset = 0;
    }
    if (out_offset == nullptr || pattern == nullptr || mask == nullptr || pattern_len == 0 ||
        (haystack_len != 0 && haystack == nullptr)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "scan_find_pattern received invalid arguments");
    }
    for (size_t index = 0; index < pattern_len; ++index) {
        if (mask[index] != 0x00 && mask[index] != 0xFF) {
            return fail(SAO_ERR_INVALID_ARGUMENT,
                        "scan_find_pattern received an invalid mask byte");
        }
    }
    if (pattern_len > haystack_len) {
        return fail(SAO_ERR_NOT_FOUND, "scan_find_pattern pattern is larger than the input");
    }

    const size_t last_start = haystack_len - pattern_len;
    for (size_t offset = 0; offset <= last_start; ++offset) {
        bool matches = true;
        for (size_t pattern_index = 0; pattern_index < pattern_len; ++pattern_index) {
            if (mask[pattern_index] == 0xFF &&
                haystack[offset + pattern_index] != pattern[pattern_index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            *out_offset = offset;
            return SAO_OK;
        }
    }
    return fail(SAO_ERR_NOT_FOUND, "scan_find_pattern found no match");
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_scan_find_aligned_u64(
    const uint8_t* haystack, size_t haystack_len, const uint64_t* target_values,
    size_t target_count, size_t* out_offsets, size_t max_out_offsets, size_t* out_match_count) {
    if (out_match_count != nullptr) {
        *out_match_count = 0;
    }
    if (max_out_offsets > std::numeric_limits<size_t>::max() / sizeof(*out_offsets)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "scan_find_aligned_u64 output capacity overflows");
    }
    if (out_offsets != nullptr && max_out_offsets != 0) {
        std::memset(out_offsets, 0, max_out_offsets * sizeof(*out_offsets));
    }
    if (out_match_count == nullptr || target_values == nullptr || target_count == 0 ||
        (haystack_len != 0 && haystack == nullptr) ||
        (out_offsets == nullptr && max_out_offsets != 0)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "scan_find_aligned_u64 received invalid arguments");
    }

    size_t required_matches = 0;
    for (size_t offset = 0; offset <= haystack_len && haystack_len - offset >= sizeof(uint64_t);
         offset += sizeof(uint64_t)) {
        uint64_t value = 0;
        std::memcpy(&value, haystack + offset, sizeof(value));
        if (is_target_value(value, target_values, target_count)) {
            ++required_matches;
        }
    }
    *out_match_count = required_matches;
    if (out_offsets == nullptr) {
        return SAO_OK;
    }
    if (max_out_offsets < required_matches) {
        return fail(SAO_ERR_BUFFER_TOO_SMALL, "scan_find_aligned_u64 output buffer is too small");
    }

    size_t write_index = 0;
    for (size_t offset = 0; offset <= haystack_len && haystack_len - offset >= sizeof(uint64_t);
         offset += sizeof(uint64_t)) {
        uint64_t value = 0;
        std::memcpy(&value, haystack + offset, sizeof(value));
        if (is_target_value(value, target_values, target_count)) {
            out_offsets[write_index++] = offset;
        }
    }
    return SAO_OK;
}
