// SAO Auto — capability and implementation gap-closure test support helpers.
//
// These helpers keep the three gap tests (core/engine/net) DRY without
// forcing every test binary to duplicate boiler-plate.  They live in a
// dedicated STATIC library ``sao_core_gap_support_lib`` so the same object code
// is linked into every gap test and stays out of the shipping DLLs.
//
// Scope: mock-only.  Nothing here spawns drivers, opens pcap adapters,
// runs real zstd compression streams, or otherwise crosses the audit
// boundary set by PLAN §1.5.  Every helper is hermetic — it either
// synthesises data locally or exercises pure-ABI queries.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sao/core/status.h"

namespace sao::core_gap {

// ---------------------------------------------------------------------------
// Classification tag — mirrors the capability/implementation/legacy matrix so tests can
// assert an entry point returns the *right* code for the right reason.
// ---------------------------------------------------------------------------
enum class GapKind : std::uint32_t {
    // Category A — compile-time / OS / vendored dependency capability gate.
    // Expected code: SAO_STATUS_ERR_CAPABILITY_MISSING.
    CapabilityGate,
    // Category B — real cross-platform implementation exists.  Callers
    // should be able to drive the entry point to SAO_STATUS_OK on any
    // supported host.
    RealImplementation,
    // Category C — legacy stub retained for binary compatibility.  Expected
    // code: SAO_STATUS_ERR_NOT_IMPLEMENTED (and the header marks the entry
    // point [[deprecated]]).
    LegacyStub,
};

// Human-readable tag for CAPTURE output in Catch2 reports.
const char* gap_kind_label(GapKind kind) noexcept;

// Assert helper — returns true when the observed status matches the
// expected code for a gap kind.  Kept as a free function so the caller can
// wrap it in Catch2 CHECK/REQUIRE and keep the assertion count truthful.
bool matches_gap_kind(sao_status_t observed, GapKind expected) noexcept;

// ---------------------------------------------------------------------------
// Deterministic test bytes.
//
// Provides a stable, self-contained byte string for hash / codec / varint
// tests.  Using a fixed pattern avoids pulling in fixture JSON and keeps
// the gap tests hermetic (no fixture disk I/O).
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> make_pattern_bytes(std::size_t byte_count);

// Convert a UTF-8 test string into a std::wstring by walking the codec
// helpers under test.  Returns an empty string on failure — the caller is
// expected to CHECK the returned size independently.
std::wstring wide_from_utf8(const std::string& utf8);

// Encoded byte length (including trailing NUL) that the codec should
// produce for a given UTF-16 input.  Used to compare probe / write
// results.
std::size_t utf8_byte_count_for(const std::wstring& utf16);

}  // namespace sao::core_gap
