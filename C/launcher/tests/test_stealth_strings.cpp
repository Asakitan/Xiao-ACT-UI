// SAO Auto — launcher/tests/test_stealth_strings.cpp
//
// Regression gate: verify that the plaintext product-identifying string
// literals removed by the launcher stealth hardening pass do NOT reappear
// in the shipped SaoAuto.exe (or any other artefact under the same build
// tree that consumes the reworked TUs).  This test scans the raw PE image
// for both narrow and wide encodings of the historical identifiers.
//
// The test is a filter — it does not attempt to prove which subsystem
// leaked the string; a red result means one of the SAO_ENC_STR() wraps
// was regressed to a plaintext literal, or a new subsystem re-introduced
// the plaintext identifier.  Investigate with `strings` / a hex viewer.
//
// When the SaoAuto.exe path is not available at compile time (e.g., the
// launcher exe wasn't built in this configure), the test degrades to a
// Catch2 SUCCEED("... — skipped") instead of failing.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The historical plaintext identifiers, in the narrow encoding they were
// emitted in the source.  wchar_t literals are widened at runtime below.
constexpr std::array<const char*, 6> kNarrowForbiddenIdentifiers = {{
    "SaoAuto.Instance",
    "SaoAutoLauncherDualRun",
    "SaoAuto.Launcher.UserMenu",
    "SaoLegacyCoreLayeredWindow",
    "SaoAutoMsgHwnd",
    "wpcap.dll",
}};

// Convert an ASCII identifier to its UTF-16LE byte sequence (matching the
// wchar_t literal layout on Windows PE).  Every wchar_t is emitted as
// two bytes: low byte first, high byte zero.
std::vector<std::uint8_t> to_utf16le_bytes(const char* ascii) {
    std::vector<std::uint8_t> out;
    for (const char* p = ascii; *p != '\0'; ++p) {
        out.push_back(static_cast<std::uint8_t>(*p));
        out.push_back(0);
    }
    return out;
}

bool contains_byte_sequence(const std::vector<std::uint8_t>& haystack,
                            const void* needle,
                            std::size_t needle_len) {
    if (needle_len == 0 || haystack.size() < needle_len) return false;
    const auto* needle_bytes = static_cast<const std::uint8_t*>(needle);
    const std::size_t stop = haystack.size() - needle_len;
    for (std::size_t i = 0; i <= stop; ++i) {
        if (std::memcmp(haystack.data() + i, needle_bytes, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

bool load_file(const char* path, std::vector<std::uint8_t>& bytes_out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const auto end_pos = stream.tellg();
    if (end_pos < 0) return false;
    const auto size = static_cast<std::size_t>(end_pos);
    stream.seekg(0, std::ios::beg);
    bytes_out.assign(size, 0);
    if (size == 0) return true;
    stream.read(reinterpret_cast<char*>(bytes_out.data()),
                static_cast<std::streamsize>(size));
    return stream.good() || stream.eof();
}

// Scan one artefact for every forbidden identifier.  Any hit fails the
// enclosing REQUIRE with a diagnostic naming which artefact and which
// identifier still leaks plaintext.
void assert_no_plaintext_identifiers(const std::string& artefact_label,
                                     const std::vector<std::uint8_t>& image) {
    for (const char* identifier : kNarrowForbiddenIdentifiers) {
        const std::size_t narrow_len = std::strlen(identifier);
        const bool narrow_hit = contains_byte_sequence(
            image, identifier, narrow_len);
        INFO("narrow identifier still resident in " << artefact_label
             << ": " << identifier);
        REQUIRE_FALSE(narrow_hit);

        const std::vector<std::uint8_t> wide = to_utf16le_bytes(identifier);
        const bool wide_hit = contains_byte_sequence(
            image, wide.data(), wide.size());
        INFO("wide identifier still resident in " << artefact_label
             << ": " << identifier);
        REQUIRE_FALSE(wide_hit);
    }
}

} // namespace

TEST_CASE("SaoAuto.exe carries no plaintext product identifiers",
          "[launcher][stealth][regression]") {
#if defined(SAO_STEALTH_SCAN_BINARY_PATH)
    const char* exe_path = SAO_STEALTH_SCAN_BINARY_PATH;
    if (exe_path == nullptr || exe_path[0] == '\0') {
        SUCCEED("stealth-strings — SaoAuto.exe path not provided at "
                "configure time; skipping");
        return;
    }

    std::vector<std::uint8_t> image;
    if (!load_file(exe_path, image)) {
        SUCCEED("stealth-strings — SaoAuto.exe not present at "
                "configured path; skipping");
        return;
    }
    REQUIRE(image.size() > 1024u); // sanity: a real PE image
    assert_no_plaintext_identifiers("SaoAuto.exe", image);
#else
    SUCCEED("stealth-strings — SaoAuto.exe path define missing; "
            "skipping (build launcher first)");
#endif
}

TEST_CASE("sao_platform_net.dll carries no plaintext wpcap identifiers",
          "[launcher][stealth][regression][platform_net]") {
#if defined(SAO_STEALTH_SCAN_PLATFORM_NET_PATH)
    const char* dll_path = SAO_STEALTH_SCAN_PLATFORM_NET_PATH;
    if (dll_path == nullptr || dll_path[0] == '\0') {
        SUCCEED("stealth-strings — sao_platform_net.dll path not provided "
                "at configure time; skipping");
        return;
    }

    std::vector<std::uint8_t> image;
    if (!load_file(dll_path, image)) {
        SUCCEED("stealth-strings — sao_platform_net.dll not present at "
                "configured path; skipping");
        return;
    }
    REQUIRE(image.size() > 1024u);
    assert_no_plaintext_identifiers("sao_platform_net.dll", image);
#else
    SUCCEED("stealth-strings — sao_platform_net.dll path define missing; "
            "skipping (build platform/net first)");
#endif
}

TEST_CASE("sao_core.dll carries no plaintext legacy layered class",
          "[launcher][stealth][regression][core]") {
#if defined(SAO_STEALTH_SCAN_CORE_PATH)
    const char* dll_path = SAO_STEALTH_SCAN_CORE_PATH;
    if (dll_path == nullptr || dll_path[0] == '\0') {
        SUCCEED("stealth-strings — sao_core.dll path not provided at "
                "configure time; skipping");
        return;
    }

    std::vector<std::uint8_t> image;
    if (!load_file(dll_path, image)) {
        SUCCEED("stealth-strings — sao_core.dll not present at "
                "configured path; skipping");
        return;
    }
    REQUIRE(image.size() > 1024u);
    assert_no_plaintext_identifiers("sao_core.dll", image);
#else
    SUCCEED("stealth-strings — sao_core.dll path define missing; "
            "skipping (build core first)");
#endif
}
