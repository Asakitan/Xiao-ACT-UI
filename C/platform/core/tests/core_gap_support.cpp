// SAO Auto — capability and implementation gap-closure test support implementation.

#include "core_gap_support.h"

#include "sao/core/string.h"

namespace sao::core_gap {

const char* gap_kind_label(GapKind kind) noexcept {
    switch (kind) {
        case GapKind::CapabilityGate:     return "A/capability_gate";
        case GapKind::RealImplementation: return "B/real_implementation";
        case GapKind::LegacyStub:         return "C/legacy_stub";
    }
    return "unknown";
}

bool matches_gap_kind(sao_status_t observed, GapKind expected) noexcept {
    switch (expected) {
        case GapKind::CapabilityGate:
            return observed == SAO_STATUS_ERR_CAPABILITY_MISSING;
        case GapKind::RealImplementation:
            // A real implementation must not report NOT_IMPLEMENTED or
            // CAPABILITY_MISSING; OK is the happy path but a specific
            // invalid-argument reject is still acceptable for probe-style
            // calls that intentionally supply nonsense inputs.
            return observed != SAO_STATUS_ERR_NOT_IMPLEMENTED &&
                   observed != SAO_STATUS_ERR_CAPABILITY_MISSING;
        case GapKind::LegacyStub:
            return observed == SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }
    return false;
}

std::vector<std::uint8_t> make_pattern_bytes(std::size_t byte_count) {
    std::vector<std::uint8_t> out;
    out.reserve(byte_count);
    // A stable low-entropy pattern — bytewise sum can be recomputed from
    // (byte_count * 127.5).  Low entropy is fine for hash / codec / varint
    // tests since they don't need distribution properties.
    for (std::size_t index = 0; index < byte_count; ++index) {
        out.push_back(static_cast<std::uint8_t>((index * 31u + 7u) & 0xFFu));
    }
    return out;
}

std::wstring wide_from_utf8(const std::string& utf8) {
    std::size_t needed = 0;
    if (sao_core_string_utf8_to_utf16(utf8.c_str(), nullptr, 0, &needed) !=
        SAO_STATUS_OK) {
        return {};
    }
    std::wstring out(needed, L'\0');
    if (sao_core_string_utf8_to_utf16(
            utf8.c_str(), out.data(), out.size(), &needed) != SAO_STATUS_OK) {
        return {};
    }
    // The probed size includes the trailing NUL, but std::wstring already
    // tracks its own terminator — strip it so std::wstring::size() reflects
    // the caller-visible length.
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

std::size_t utf8_byte_count_for(const std::wstring& utf16) {
    std::size_t needed = 0;
    if (sao_core_string_utf16_to_utf8(utf16.c_str(), nullptr, 0, &needed) !=
        SAO_STATUS_OK) {
        return 0;
    }
    return needed;
}

}  // namespace sao::core_gap
