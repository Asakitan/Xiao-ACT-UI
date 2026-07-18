#pragma once

// SHA-256 helper for export/import envelope integrity.
//
// Wraps the Windows CNG (`bcrypt.h`) BCryptHash primitive so the AI Editor
// runtime can stamp each export payload with a checksum and verify it on
// import without pulling in an external crypto library.  Two shapes are
// exposed:
//   * sha256_hex(bytes)   — bytes → 64-char lower-case hex string
//   * sha256_envelope_hex — serialise the payload with `sha256` stripped,
//     then hash the resulting canonical JSON dump.  Used by both the
//     export path (stamp) and the import path (verify).

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {

using Json = nlohmann::json;

// Return the hex-encoded SHA-256 digest of `bytes` (lowercase, 64 chars).
// Empty input returns the empty-hash constant.  On any CNG failure the
// returned string is empty — callers must treat empty as "hashing unavailable"
// rather than "digest is zero".
std::string sha256_hex(std::string_view bytes);

// Compute the SHA-256 of the payload's canonical JSON serialisation *with the
// `sha256` field removed* so a stamped envelope hashes identically before and
// after the stamp is added.  We rely on nlohmann::json object ordering
// (sorted by key when serialised) to keep the hash stable across the
// export→import round trip.
std::string sha256_envelope_hex(const Json& payload);

}  // namespace sao::ai_editor::native
