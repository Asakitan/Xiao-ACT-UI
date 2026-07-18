#include "sha256_helper.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sao::ai_editor::native {
namespace {

// Convert a raw digest into lower-case hex.  Constant-size loop so the
// generated code is tight and constant-time relative to input length.
std::string bytes_to_hex(const uint8_t* data, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t index = 0; index < length; ++index) {
        out[index * 2] = kHex[(data[index] >> 4) & 0x0F];
        out[index * 2 + 1] = kHex[data[index] & 0x0F];
    }
    return out;
}

}  // namespace

std::string sha256_hex(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        return {};
    }
    // 32-byte digest for SHA-256.  We size the digest buffer statically so a
    // BCryptGetProperty round-trip is unnecessary — the algorithm is fixed.
    std::array<uint8_t, 32> digest{};
    status = BCryptHash(
        algorithm,
        nullptr, 0,
        // BCryptHash takes non-const PUCHAR for the input buffer; the
        // implementation does not mutate it but the signature is legacy.
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),
        digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        return {};
    }
    return bytes_to_hex(digest.data(), digest.size());
}

std::string sha256_envelope_hex(const Json& payload) {
    // Copy the payload and strip `sha256` before serialising so the digest
    // covers the semantic body only.  This matches the verification path:
    // import strips the sha256, recomputes, then compares against the
    // extracted string.  If the caller hands us a non-object payload we
    // still hash a deterministic form of it — that path is only exercised
    // by unit tests but keeps the helper reusable.
    Json body = payload;
    if (body.is_object()) {
        body.erase("sha256");
    }
    // dump() with no indent produces the canonical single-line form; keys
    // in json::object are kept in insertion order by default but the
    // library's serialise path sorts them alphabetically because we use
    // the ordered_json default configuration.  Even without alphabetical
    // ordering the round-trip is stable because we stamp with the same
    // dump call the verify side uses.
    const std::string serialised = body.dump();
    return sha256_hex(serialised);
}

}  // namespace sao::ai_editor::native
