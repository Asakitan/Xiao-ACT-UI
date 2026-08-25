// SAO Auto -- runtime installer integrity internals.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "sao/core/status.h"

namespace sao::runtime_installer::internal {

// Rolling SHA-256 (+ optional BLAKE3) hasher used by the download loop.
// The BLAKE3 half is opt-in per manifest entry.
class HashPipeline {
public:
    HashPipeline();
    ~HashPipeline();
    HashPipeline(const HashPipeline&) = delete;
    HashPipeline& operator=(const HashPipeline&) = delete;
    HashPipeline(HashPipeline&&) = delete;
    HashPipeline& operator=(HashPipeline&&) = delete;

    sao_status_t begin(bool include_blake3);
    sao_status_t update(const uint8_t* bytes, size_t length);
    sao_status_t finish(std::string& sha256_hex,
                        std::string& blake3_hex);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Constant-time hex string comparison.  Returns SAO_STATUS_OK when equal,
// SAO_STATUS_ERR_UNKNOWN otherwise (used to signal tampering to callers
// without leaking mismatched bytes).
sao_status_t compare_hex_equal(const std::string& lhs,
                               const std::string& rhs) noexcept;

sao_status_t hash_file_sha256(const std::wstring& path,
                              std::string& sha256_hex);

sao_status_t hash_directory_tree_sha256(const std::wstring& root,
                                        std::string& sha256_hex);

}  // namespace sao::runtime_installer::internal
