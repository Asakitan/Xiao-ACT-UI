// SAO Auto -- runtime installer private manifest model.
//
// Kept out of include/ so unrelated modules cannot include it.  The public
// C ABI never exposes ManifestEntry directly -- all access goes through
// opaque handles + copy-out helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sao/core/status.h"
#include "sao/runtime_installer/manifest.h"
#include "sao/runtime_installer/runtime_installer.h"

namespace sao::runtime_installer::internal {

struct ManifestEntry {
    sao_runtime_kind_t         kind = SAO_RUNTIME_KIND_INVALID;
    std::string                version;
    std::string                url;
    std::vector<std::string>   mirrors;
    std::string                sha256_hex;
    std::string                blake3_hex;   // optional; empty when not set
    uint64_t                   size_bytes = 0;
    sao_runtime_archive_t      archive = SAO_RUNTIME_ARCHIVE_INVALID;
    sao_runtime_install_hint_t install_hint = SAO_RUNTIME_INSTALL_HINT_NONE;
};

struct Manifest {
    uint32_t                    schema = 0;
    std::string                 version;
    std::vector<ManifestEntry>  entries;
};

sao_status_t parse_manifest(const char* data,
                            size_t length,
                            Manifest& out) noexcept;

const ManifestEntry* find_entry(const Manifest& manifest,
                                sao_runtime_kind_t kind) noexcept;

}  // namespace sao::runtime_installer::internal
