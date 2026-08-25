// Encrypted native image loading through the owned reflective-loader API.

#include "sao/plugins/loader/native_loader_encrypted.h"
#include "sao_security/crypto/aes.h"
#include "sao_security/loader/reflective_dll.h"
#include "sao_core/sao_status.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sao::plugins::loader::native_loader {

namespace {

constexpr uint64_t kMaxEncryptedBlobBytes = 512ULL * 1024ULL * 1024ULL;
constexpr size_t kNonceBytes = 12;
constexpr size_t kTagBytes = 16;
constexpr size_t kEnvelopeHeaderBytes = kNonceBytes + kTagBytes;

void secure_wipe(void* data, size_t size) noexcept {
    if (data == nullptr || size == 0)
        return;
#if defined(_WIN32)
    SecureZeroMemory(data, size);
#else
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = 0;
#endif
}

struct SecureKey {
    std::array<uint8_t, 32> bytes{};
    ~SecureKey() { secure_wipe(bytes.data(), bytes.size()); }
};

struct SecureBlob {
    std::vector<uint8_t> bytes;
    ~SecureBlob() { secure_wipe(bytes.data(), bytes.size()); }
};

bool parse_hex_key(const char* hex, std::array<uint8_t, 32>& key) noexcept {
    if (hex == nullptr || strnlen_s(hex, 65u) != 64u)
        return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < key.size(); ++i) {
        const int hi = value(hex[i * 2]);
        const int lo = value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    uint8_t any_nonzero = 0;
    for (const auto byte : key)
        any_nonzero |= byte;
    return any_nonzero != 0;
}

int read_bounded_blob(const char* path, SecureBlob& blob) noexcept {
    if (path == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return SAO_ERR_OS_CALL_FAILED;
    const std::streampos end = input.tellg();
    if (end < 0)
        return SAO_ERR_OS_CALL_FAILED;
    const uint64_t size = static_cast<uint64_t>(end);
    if (size < kEnvelopeHeaderBytes + 1 || size > kMaxEncryptedBlobBytes ||
        size > std::numeric_limits<size_t>::max()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    blob.bytes.resize(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(blob.bytes.data()),
               static_cast<std::streamsize>(blob.bytes.size()));
    if (!input || static_cast<uint64_t>(input.gcount()) != size)
        return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int sao_plugins_native_loader_load_encrypted(
    const char* encrypted_path, const char* key_hex, void** out_module_handle) {
    if (out_module_handle != nullptr)
        *out_module_handle = nullptr;
    if (encrypted_path == nullptr || key_hex == nullptr || out_module_handle == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        SecureKey key;
        if (!parse_hex_key(key_hex, key.bytes))
            return SAO_ERR_INVALID_ARGUMENT;

        SecureBlob blob;
        const int read_status = read_bounded_blob(encrypted_path, blob);
        if (read_status != SAO_OK)
            return read_status;
        const uint8_t* nonce = blob.bytes.data();
        uint8_t* tag = blob.bytes.data() + kNonceBytes;
        const uint8_t* ciphertext = blob.bytes.data() + kEnvelopeHeaderBytes;
        const size_t ciphertext_size = blob.bytes.size() - kEnvelopeHeaderBytes;
        if (ciphertext_size > UINT32_MAX)
            return SAO_ERR_INVALID_ARGUMENT;

        SecureBlob plaintext;
        plaintext.bytes.resize(ciphertext_size);
        SaoAesGcmParams params{};
        params.key = key.bytes.data();
        params.key_len = static_cast<uint32_t>(key.bytes.size());
        params.iv = nonce;
        params.iv_len = static_cast<uint32_t>(kNonceBytes);
        params.aad = nullptr;
        params.aad_len = 0;
        params.tag = tag;
        params.tag_len = static_cast<uint32_t>(kTagBytes);
        const int32_t decrypt_status = sao_security_crypto_aes256_gcm_decrypt(
            &params, ciphertext, static_cast<uint32_t>(ciphertext_size),
            plaintext.bytes.data(), static_cast<uint32_t>(ciphertext_size));
        if (decrypt_status != SAO_OK)
            return decrypt_status;

        sao_security_loader_module_v1 module{
            sizeof(sao_security_loader_module_v1),
            SAO_SECURITY_LOADER_MODULE_API_VERSION_1,
            nullptr,
            nullptr,
            0,
            0,
            SAO_SECURITY_LOADER_MODULE_ATTACH_NOT_INVOKED,
            0,
        };
        const int32_t load_status = sao_security_loader_reflective_load_v1(
            plaintext.bytes.data(), static_cast<uint32_t>(plaintext.bytes.size()), &module);
        if (load_status != SAO_OK)
            return load_status;
        if (module.image_base == nullptr)
            return SAO_ERR_UNKNOWN;
        const int32_t attach_status =
            sao_security_loader_module_invoke_process_attach_v1(&module, nullptr);
        if (attach_status != SAO_OK) {
            int32_t cleanup_status = SAO_OK;
            if (module.image_base != nullptr) {
                cleanup_status = sao_security_loader_release_image_quiescent_v1(
                    module.image_base);
            }
            return cleanup_status != SAO_OK ? cleanup_status : attach_status;
        }
        if (module.image_base == nullptr)
            return SAO_ERR_UNKNOWN;
        *out_module_handle = module.image_base;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int sao_plugins_native_loader_unload_encrypted(
    void* module_handle) {
    if (module_handle == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return sao_security_loader_release_image_quiescent_v1(module_handle);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::loader::native_loader
