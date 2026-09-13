#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/shell/stub/runtime.h"
#include "sao_core/sao_status.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class IdentityPinStatus {
    Ok,
    Unavailable,
    Malformed,
};

int hex_nibble(uint8_t value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool all_zero(const uint8_t* value, size_t size) noexcept
{
    uint8_t aggregate = 0;
    for (size_t index = 0; index < size; ++index) aggregate |= value[index];
    return aggregate == 0U;
}

bool all_zero(const std::array<uint8_t, 32>& value) noexcept
{
    return all_zero(value.data(), value.size());
}

bool constant_time_equal(const std::array<uint8_t, 32>& expected,
                         const uint8_t actual[32]) noexcept
{
    uint8_t difference = 0;
    for (size_t index = 0; index < expected.size(); ++index) {
        difference |= static_cast<uint8_t>(expected[index] ^ actual[index]);
    }
    return difference == 0U;
}

IdentityPinStatus read_identity_pin(
    const std::wstring& path,
    std::array<uint8_t, 32>& out) noexcept
{
    // Legacy shell.metadata is a raw 32-byte identity or its 64-hex encoding.
    out.fill(0U);
    if (path.empty()) return IdentityPinStatus::Unavailable;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return IdentityPinStatus::Unavailable;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 128) {
        CloseHandle(file);
        return IdentityPinStatus::Malformed;
    }
    std::array<uint8_t, 128> bytes{};
    DWORD received = 0;
    const DWORD expected = static_cast<DWORD>(size.QuadPart);
    const bool read = ReadFile(file, bytes.data(), expected, &received, nullptr) != FALSE;
    CloseHandle(file);
    if (!read || received != expected) return IdentityPinStatus::Unavailable;
    if (received == static_cast<DWORD>(out.size())) {
        std::memcpy(out.data(), bytes.data(), out.size());
        return all_zero(out) ? IdentityPinStatus::Malformed : IdentityPinStatus::Ok;
    }
    size_t first = 0;
    size_t last = received;
    const auto whitespace = [](uint8_t value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    };
    while (first < last && whitespace(bytes[first])) ++first;
    while (last > first && whitespace(bytes[last - 1U])) --last;
    if (last - first != out.size() * 2U) return IdentityPinStatus::Malformed;
    for (size_t index = 0; index < out.size(); ++index) {
        const int high = hex_nibble(bytes[first + index * 2U]);
        const int low = hex_nibble(bytes[first + index * 2U + 1U]);
        if (high < 0 || low < 0) {
            out.fill(0U);
            return IdentityPinStatus::Malformed;
        }
        out[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return all_zero(out) ? IdentityPinStatus::Malformed : IdentityPinStatus::Ok;
}

const char* integrity_reason(uint32_t reason) noexcept
{
    switch (reason) {
    case SAO_SHELL_STUB_INTEGRITY_REASON_PROVIDER_MISSING:
        return "shell runtime provider missing";
    case SAO_SHELL_STUB_INTEGRITY_REASON_PROVIDER_INCOMPLETE:
        return "shell runtime provider incomplete";
    case SAO_SHELL_STUB_INTEGRITY_REASON_METADATA_UNAVAILABLE:
        return "shell runtime metadata unavailable";
    case SAO_SHELL_STUB_INTEGRITY_REASON_METADATA_INVALID:
        return "shell runtime metadata invalid";
    case SAO_SHELL_STUB_INTEGRITY_REASON_IMAGE_IDENTITY_MISMATCH:
        return "shell runtime image identity mismatch";
    case SAO_SHELL_STUB_INTEGRITY_REASON_REGION_SCOPE_INVALID:
        return "shell runtime metadata scope invalid";
    case SAO_SHELL_STUB_INTEGRITY_REASON_MANIFEST_UNVERIFIED:
        return "shell manifest not verified by runtime";
    case SAO_SHELL_STUB_INTEGRITY_REASON_MANIFEST_REJECTED:
        return "shell manifest verification rejected";
    case SAO_SHELL_STUB_INTEGRITY_REASON_MANIFEST_IDENTITY_MISMATCH:
        return "shell manifest identity mismatch";
    case SAO_SHELL_STUB_INTEGRITY_REASON_HASH_UNVERIFIED:
        return "shell section hashes not verified by runtime";
    case SAO_SHELL_STUB_INTEGRITY_REASON_REGION_HASH_MISMATCH:
        return "shell region hash mismatch";
    case SAO_SHELL_STUB_INTEGRITY_REASON_PROVIDER_CHANGED:
        return "shell runtime provider changed during verification";
    case SAO_SHELL_STUB_INTEGRITY_REASON_RECOVERY_REQUIRED:
        return "shell runtime recovery required";
    default:
        return "shell integrity runtime failure";
    }
}

sao_status_t fail(sao_shell_verify_result* out, const char* reason) noexcept
{
    out->tampered = 1;
    strncpy_s(out->reason, sizeof(out->reason), reason, _TRUNCATE);
    return SAO_STATUS_SHELL_TAMPERED;
}

} // namespace

extern "C" sao_status_t sao_shell_verify_integrity(
    sao_shell_verify_result* out) {
    if (out == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    *out = {};
    try {
        const auto configuration =
            sao::launcher::launcherProviderConfigurationSnapshot().shell;
        if (!configuration.enabled) {
            return fail(out, "shell provider is disabled");
        }
        if (configuration.metadata_path.empty()) {
            return fail(out, "shell metadata identity pin unavailable");
        }
        sao_shell_stub_integrity_report_t report{};
        report.abi_version = SAO_SHELL_STUB_INTEGRITY_REPORT_ABI_VERSION;
        report.struct_size = static_cast<uint32_t>(sizeof(report));
        const auto status = sao_shell_stub_runtime_verify_integrity(&report);
        if (status != SAO_OK ||
            report.abi_version != SAO_SHELL_STUB_INTEGRITY_REPORT_ABI_VERSION ||
            report.struct_size < static_cast<uint32_t>(sizeof(report)) ||
            report.reason != SAO_SHELL_STUB_INTEGRITY_REASON_NONE ||
            report.provider_configured != 1U || report.provider_generation == 0U ||
            report.image_base == 0U || report.image_size == 0U ||
            report.region_count < 2U ||
            report.region_count > SAO_SHELL_STUB_MAX_SECTIONS + 1U ||
            report.metadata_valid != 1U || report.image_identity_valid != 1U ||
            report.scope_valid != 1U || report.manifest_verified != 1U ||
            report.hashes_verified != 1U ||
            all_zero(report.variant_tag, sizeof(report.variant_tag)) ||
            all_zero(report.build_id, sizeof(report.build_id)) ||
            all_zero(report.metadata_identity_blake3,
                     sizeof(report.metadata_identity_blake3))) {
            return fail(out, integrity_reason(report.reason));
        }
        std::array<uint8_t, 32> expected_identity{};
        const auto pin_status = read_identity_pin(
            configuration.metadata_path, expected_identity);
        if (pin_status == IdentityPinStatus::Unavailable) {
            return fail(out, "shell metadata identity pin unavailable");
        }
        if (pin_status == IdentityPinStatus::Malformed) {
            return fail(out, "shell metadata identity pin malformed");
        }
        if (!constant_time_equal(expected_identity,
                                 report.metadata_identity_blake3)) {
            return fail(out, "shell metadata identity pin mismatch");
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return fail(out, "shell provider failed");
    }
}

extern "C" sao_status_t sao_shell_shutdown(void) {
    try {
        return sao_shell_stub_runtime_request_clear_provider() == SAO_OK
                   ? SAO_STATUS_OK
                   : SAO_STATUS_INTERNAL;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}
