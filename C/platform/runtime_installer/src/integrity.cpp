// SAO Auto -- runtime installer integrity subsystem.
//
// SHA-256 rolls in lockstep with the download loop via BCryptCreateHash /
// BCRYPT_SHA256_ALGORITHM.  We chose SHA-256 as the mandatory integrity
// primitive because every runtime distribution (python.org, dotnet.microsoft
// .com, lua.org, angelcode.com) ships a SHA-256 attestation and BCrypt is
// available on every supported Windows SKU with no extra link deps.
//
// BLAKE3 runs in lockstep with SHA-256 for downloaded runtime payloads.

#include "integrity_internal.h"
#include "sao_security/crypto/blake3.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
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

namespace fs = std::filesystem;

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

bool is_reparse_point(const fs::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool has_parent_component(const fs::path& path) noexcept {
    for (const auto& component : path) {
        if (component == L"..") return true;
    }
    return false;
}

sao_status_t update_u64(HashPipeline& pipe, uint64_t value) {
    std::array<uint8_t, sizeof(uint64_t)> encoded{};
    for (size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] = static_cast<uint8_t>((value >> (i * 8u)) & 0xffu);
    }
    return pipe.update(encoded.data(), encoded.size());
}

struct TreeFile {
    fs::path path;
    std::string relative;
};

}  // namespace

class HashPipeline::Impl {
public:
    Impl() = default;
    ~Impl() {
        if (blake3_ != nullptr) {
            std::array<uint8_t, 32> discard{};
            (void)sao_security_crypto_blake3_final(blake3_, discard.data());
            blake3_ = nullptr;
        }
    }

    sao_status_t init(bool include_blake3) {
        BCRYPT_ALG_HANDLE raw_alg = nullptr;
        auto s = ::BCryptOpenAlgorithmProvider(
            &raw_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha_alg_.reset(raw_alg);
        BCRYPT_HASH_HANDLE raw_hash = nullptr;
        s = ::BCryptCreateHash(sha_alg_.get(), &raw_hash, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha_hash_.reset(raw_hash);
        include_blake3_ = include_blake3;
        if (include_blake3_ &&
            sao_security_crypto_blake3_init(&blake3_) != SAO_STATUS_OK) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        return SAO_STATUS_OK;
    }

    sao_status_t update(const uint8_t* bytes, size_t length) {
        if (length == 0) return SAO_STATUS_OK;
        const auto s = ::BCryptHashData(
            sha_hash_.get(),
            const_cast<PUCHAR>(bytes),
            static_cast<ULONG>(length),
            0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        if (include_blake3_) {
            size_t offset = 0u;
            while (offset < length) {
                const size_t remaining = length - offset;
                const uint32_t chunk = static_cast<uint32_t>(
                    std::min<size_t>(remaining, UINT32_MAX));
                if (sao_security_crypto_blake3_update(
                        blake3_, bytes + offset, chunk) != SAO_STATUS_OK) {
                    return SAO_STATUS_ERR_OS_CALL_FAILED;
                }
                offset += chunk;
            }
        }
        return SAO_STATUS_OK;
    }

    sao_status_t finalize(std::string& sha256_hex,
                          std::string& blake3_hex) {
        std::array<uint8_t, 32> sha_digest{};
        const auto s = ::BCryptFinishHash(
            sha_hash_.get(),
            sha_digest.data(),
            static_cast<ULONG>(sha_digest.size()),
            0);
        if (!BCRYPT_SUCCESS(s)) return SAO_STATUS_ERR_OS_CALL_FAILED;
        sha256_hex = to_hex_lower(sha_digest.data(), sha_digest.size());
        if (include_blake3_) {
            std::array<uint8_t, 32> blake_digest{};
            sao_blake3_ctx_t* context = blake3_;
            blake3_ = nullptr;
            if (sao_security_crypto_blake3_final(
                    context, blake_digest.data()) != SAO_STATUS_OK) {
                return SAO_STATUS_ERR_OS_CALL_FAILED;
            }
            blake3_hex = to_hex_lower(
                blake_digest.data(), blake_digest.size());
        } else {
            blake3_hex.clear();
        }
        return SAO_STATUS_OK;
    }

private:
    std::unique_ptr<void, BCryptAlgDeleter>  sha_alg_;
    std::unique_ptr<void, BCryptHashDeleter> sha_hash_;
    sao_blake3_ctx_t* blake3_ = nullptr;
    bool include_blake3_ = false;
};

HashPipeline::HashPipeline() : impl_(std::make_unique<Impl>()) {}
HashPipeline::~HashPipeline() = default;

sao_status_t HashPipeline::begin(bool include_blake3) {
    return impl_->init(include_blake3);
}

sao_status_t HashPipeline::update(const uint8_t* bytes, size_t length) {
    return impl_->update(bytes, length);
}

sao_status_t HashPipeline::finish(std::string& sha256_hex,
                                  std::string& blake3_hex) {
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
                              std::string& sha256_hex) {
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

sao_status_t hash_directory_tree_sha256(const std::wstring& root,
                                        std::string& sha256_hex) {
    std::error_code ec;
    const fs::path root_path(root);
    if (!fs::is_directory(root_path, ec) || ec) {
        return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_ERR_NOT_FOUND;
    }
    if (is_reparse_point(root_path)) return SAO_STATUS_ERR_UNKNOWN;

    const fs::path canonical_root = fs::weakly_canonical(root_path, ec);
    if (ec) return SAO_STATUS_ERR_UNKNOWN;

    std::vector<TreeFile> files;
    fs::recursive_directory_iterator it(root_path), end;
    for (; it != end; it.increment(ec)) {
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
        const fs::path entry_path = it->path();
        const DWORD attributes = ::GetFileAttributesW(entry_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return SAO_STATUS_ERR_OS_CALL_FAILED;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return SAO_STATUS_ERR_UNKNOWN;

        const fs::path canonical_entry = fs::weakly_canonical(entry_path, ec);
        if (ec) return SAO_STATUS_ERR_UNKNOWN;
        const fs::path relative_path = fs::relative(canonical_entry, canonical_root, ec);
        if (ec || relative_path.empty() || relative_path.is_absolute() ||
            has_parent_component(relative_path)) return SAO_STATUS_ERR_UNKNOWN;

        if (it->is_directory(ec)) {
            if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
            continue;
        }
        if (!it->is_regular_file(ec) || ec) {
            return ec ? SAO_STATUS_ERR_OS_CALL_FAILED : SAO_STATUS_ERR_UNKNOWN;
        }
        const std::string relative = relative_path.generic_string();
        if (relative.empty()) return SAO_STATUS_ERR_UNKNOWN;
        files.push_back(TreeFile{entry_path, relative});
    }

    std::sort(files.begin(), files.end(), [](const TreeFile& lhs, const TreeFile& rhs) {
        return lhs.relative < rhs.relative;
    });
    for (size_t i = 1; i < files.size(); ++i) {
        if (files[i - 1].relative == files[i].relative) return SAO_STATUS_ERR_UNKNOWN;
    }

    HashPipeline pipe;
    if (const auto s = pipe.begin(false); s != SAO_STATUS_OK) return s;
    static constexpr uint8_t kHeader[] = {
        'S', 'A', 'O', '-', 'R', 'T', '-', 'T', 'R', 'E', 'E', '-', 'S', 'H', 'A', '2',
        '5', '6', '-', 'V', '1', 0,
    };
    if (const auto s = pipe.update(kHeader, sizeof(kHeader)); s != SAO_STATUS_OK) return s;
    if (const auto s = update_u64(pipe, static_cast<uint64_t>(files.size())); s != SAO_STATUS_OK) return s;

    std::vector<uint8_t> buffer(64 * 1024);
    for (const auto& file : files) {
        std::ifstream stream(file.path, std::ios::binary);
        if (!stream.is_open()) return SAO_STATUS_ERR_ACCESS_DENIED;
        const uint64_t expected_size = fs::file_size(file.path, ec);
        if (ec) return SAO_STATUS_ERR_OS_CALL_FAILED;
        if (const auto s = update_u64(pipe, static_cast<uint64_t>(file.relative.size())); s != SAO_STATUS_OK) return s;
        if (const auto s = pipe.update(reinterpret_cast<const uint8_t*>(file.relative.data()), file.relative.size());
            s != SAO_STATUS_OK) return s;
        if (const auto s = update_u64(pipe, expected_size); s != SAO_STATUS_OK) return s;
        const uint8_t content_begin = 0x7b;
        const uint8_t content_end = 0x7d;
        if (const auto s = pipe.update(&content_begin, 1); s != SAO_STATUS_OK) return s;
        uint64_t read_total = 0;
        while (stream.good()) {
            stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto got = static_cast<size_t>(stream.gcount());
            if (got == 0) break;
            if (read_total > UINT64_MAX - static_cast<uint64_t>(got)) return SAO_STATUS_ERR_UNKNOWN;
            read_total += static_cast<uint64_t>(got);
            if (const auto s = pipe.update(buffer.data(), got); s != SAO_STATUS_OK) return s;
        }
        if (stream.bad() || read_total != expected_size) return SAO_STATUS_ERR_UNKNOWN;
        if (const auto s = pipe.update(&content_end, 1); s != SAO_STATUS_OK) return s;
    }

    std::string unused_blake3;
    return pipe.finish(sha256_hex, unused_blake3);
}

}  // namespace sao::runtime_installer::internal
