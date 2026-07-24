// SAO Auto -- runtime installer integrity subsystem.
//
// SHA-256 rolls in lockstep with the download loop via BCryptCreateHash /
// BCRYPT_SHA256_ALGORITHM.  We chose SHA-256 as the mandatory integrity
// primitive because every runtime distribution (python.org, dotnet.microsoft
// .com, lua.org, angelcode.com) ships a SHA-256 attestation and BCrypt is
// available on every supported Windows SKU with no extra link deps.
//
// The manifest schema also accepts a blake3_hex field for future
// belt-and-suspenders defence.  The current implementation validates its
// syntactic shape (64 lowercase hex chars) but does not yet re-hash the
// payload with BLAKE3 -- the sao_security_crypto DLL cannot be linked
// from platform/ because the security subdirectory is added AFTER
// platform/ in the top-level CMakeLists.  A future refactor may promote
// BLAKE3 into platform/core so the check runs alongside SHA-256; until
// then, blake3_hex is a static attestation stored in the marker.

#include "integrity_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

namespace sao::runtime_installer::internal {

namespace {

struct BCryptAlgDeleter {
    void operator()(BCRYPT_ALG_HANDLE h) const noexcept {
        if (h != nullptr) ::BCryptCloseAlgorithmProvider(h, 0);
    }
};
struct BCryptHashDeleter {
    void operator()(BCRYPT_HASH_HANDLE h) const noexcept {
        if (h != nullptr) ::BCryptDestroyHash(h);
    }
};

std::string to_hex_lower(const uint8_t* bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out[i * 2 + 0] = digits[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

bool constant_time_equal(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

class HashPipeline::Impl {
public:
    Impl() = default;
    ~Impl() = default;

    sao_status_t init(bool /*include_blake3*/) noexcept {
        BCRYPT_ALG_HANDLE raw_alg = nullptr;
        auto s = ::BCryptOpenAlgorithmProvider(
            &raw_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha_alg_.reset(raw_alg);
        BCRYPT_HASH_HANDLE raw_hash = nullptr;
        s = ::BCryptCreateHash(sha_alg_.get(), &raw_hash, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha_hash_.reset(raw_hash);
        return SAO_STATUS_OK;
    }

    sao_status_t update(const uint8_t* bytes, size_t length) noexcept {
        if (length == 0) return SAO_STATUS_OK;
        const auto s = ::BCryptHashData(
            sha_hash_.get(),
            const_cast<PUCHAR>(bytes),
            static_cast<ULONG>(length),
            0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        return SAO_STATUS_OK;
    }

    sao_status_t finalize(std::string& sha256_hex,
                          std::string& blake3_hex) noexcept {
        std::array<uint8_t, 32> sha_digest{};
        const auto s = ::BCryptFinishHash(
            sha_hash_.get(),
            sha_digest.data(),
            static_cast<ULONG>(sha_digest.size()),
            0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha256_hex = to_hex_lower(sha_digest.data(), sha_digest.size());
        // BLAKE3 is a future extension -- see file header for rationale.
        blake3_hex.clear();
        return SAO_STATUS_OK;
    }

private:
    std::unique_ptr<void, BCryptAlgDeleter>  sha_alg_;
    std::unique_ptr<void, BCryptHashDeleter> sha_hash_;
};

HashPipeline::HashPipeline() : impl_(std::make_unique<Impl>()) {}
HashPipeline::~HashPipeline() = default;

sao_status_t HashPipeline::begin(bool include_blake3) noexcept {
    return impl_->init(include_blake3);
}

sao_status_t HashPipeline::update(const uint8_t* bytes, size_t length) noexcept {
    return impl_->update(bytes, length);
}

sao_status_t HashPipeline::finish(std::string& sha256_hex,
                                  std::string& blake3_hex) noexcept {
    return impl_->finalize(sha256_hex, blake3_hex);
}

sao_status_t compare_hex_equal(const std::string& lhs,
                               const std::string& rhs) noexcept {
    if (lhs.empty() || rhs.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return constant_time_equal(lhs, rhs)
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_UNKNOWN;
}

sao_status_t hash_file_sha256(const std::wstring& path,
                              std::string& sha256_hex) noexcept {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) return SAO_STATUS_ERR_NOT_FOUND;
    HashPipeline pipe;
    if (const auto s = pipe.begin(false); s != SAO_STATUS_OK) return s;
    std::vector<uint8_t> buffer(64 * 1024);
    while (stream.good()) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<size_t>(stream.gcount());
        if (got == 0) break;
        if (const auto s = pipe.update(buffer.data(), got); s != SAO_STATUS_OK) return s;
    }
    if (stream.bad()) return SAO_STATUS_ERR_OS_CALL_FAILED;
    std::string b3;
    return pipe.finish(sha256_hex, b3);
}

}  // namespace sao::runtime_installer::internal
