#include "sao/core/crypto.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <cstring>
#include <limits>
#include <vector>

namespace {

struct AlgorithmSpec {
    const wchar_t* identifier;
    size_t digest_size;
};

bool algorithm_spec(int32_t algo, AlgorithmSpec* out_spec) {
    switch (algo) {
        case SAO_HASH_SHA256:
            *out_spec = {BCRYPT_SHA256_ALGORITHM, SAO_HASH_SHA256_BYTES};
            return true;
        case SAO_HASH_SHA512:
            *out_spec = {BCRYPT_SHA512_ALGORITHM, SAO_HASH_SHA512_BYTES};
            return true;
        case SAO_HASH_SHA1:
            *out_spec = {BCRYPT_SHA1_ALGORITHM, SAO_HASH_SHA1_BYTES};
            return true;
        default:
            return false;
    }
}

void close_algorithm(BCRYPT_ALG_HANDLE algorithm) {
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
}

void destroy_hash(BCRYPT_HASH_HANDLE hash) {
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
}

void destroy_key(BCRYPT_KEY_HANDLE key) {
    if (key != nullptr) {
        BCryptDestroyKey(key);
    }
}

sao_status_t hash_bytes(
    int32_t algo,
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_digest,
    size_t digest_capacity) {
    AlgorithmSpec spec{};
    if (!algorithm_spec(algo, &spec)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_digest == nullptr || digest_capacity < spec.digest_size ||
        (key_len != 0 && key == nullptr) || (data_len != 0 && data == nullptr) ||
        key_len > std::numeric_limits<ULONG>::max() ||
        data_len > std::numeric_limits<ULONG>::max()) {
        return digest_capacity < spec.digest_size && out_digest != nullptr
            ? SAO_STATUS_ERR_BUFFER_TOO_SMALL
            : SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm,
            spec.identifier,
            nullptr,
            key != nullptr ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0);
        if (status < 0) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        ULONG object_size = 0;
        ULONG bytes_copied = 0;
        status = BCryptGetProperty(algorithm,
                                   BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&object_size),
                                   sizeof(object_size),
                                   &bytes_copied,
                                   0);
        if (status < 0) {
            close_algorithm(algorithm);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        std::vector<uint8_t> hash_object(object_size);
        status = BCryptCreateHash(algorithm,
                                  &hash,
                                  hash_object.data(),
                                  object_size,
                                  const_cast<PUCHAR>(key),
                                  static_cast<ULONG>(key_len),
                                  0);
        if (status >= 0 && data_len != 0) {
            status = BCryptHashData(
                hash, const_cast<PUCHAR>(data), static_cast<ULONG>(data_len), 0);
        }
        if (status >= 0) {
            status = BCryptFinishHash(
                hash, out_digest, static_cast<ULONG>(spec.digest_size), 0);
        }
        destroy_hash(hash);
        close_algorithm(algorithm);
        return status >= 0 ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        destroy_hash(hash);
        close_algorithm(algorithm);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t aes_gcm_crypt(
    bool encrypt,
    const uint8_t* key32,
    const uint8_t* nonce12,
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* input,
    size_t input_len,
    uint8_t* output,
    uint8_t* tag16) {
    if (key32 == nullptr || nonce12 == nullptr || tag16 == nullptr ||
        (aad_len != 0 && aad == nullptr) || (input_len != 0 && input == nullptr) ||
        (input_len != 0 && output == nullptr) ||
        aad_len > std::numeric_limits<ULONG>::max() ||
        input_len > std::numeric_limits<ULONG>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    try {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (status < 0) {
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        status = BCryptSetProperty(
            algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
            0);
        ULONG object_size = 0;
        ULONG bytes_copied = 0;
        if (status >= 0) {
            status = BCryptGetProperty(algorithm,
                                       BCRYPT_OBJECT_LENGTH,
                                       reinterpret_cast<PUCHAR>(&object_size),
                                       sizeof(object_size),
                                       &bytes_copied,
                                       0);
        }
        if (status < 0) {
            close_algorithm(algorithm);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        std::vector<uint8_t> key_object(object_size);
        status = BCryptGenerateSymmetricKey(algorithm,
                                            &key,
                                            key_object.data(),
                                            object_size,
                                            const_cast<PUCHAR>(key32),
                                            32,
                                            0);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
        BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
        auth_info.pbNonce = const_cast<PUCHAR>(nonce12);
        auth_info.cbNonce = 12;
        auth_info.pbAuthData = const_cast<PUCHAR>(aad);
        auth_info.cbAuthData = static_cast<ULONG>(aad_len);
        auth_info.pbTag = tag16;
        auth_info.cbTag = 16;

        ULONG output_bytes = 0;
        if (status >= 0) {
            status = encrypt
                ? BCryptEncrypt(key,
                                const_cast<PUCHAR>(input),
                                static_cast<ULONG>(input_len),
                                &auth_info,
                                nullptr,
                                0,
                                output,
                                static_cast<ULONG>(input_len),
                                &output_bytes,
                                0)
                : BCryptDecrypt(key,
                                const_cast<PUCHAR>(input),
                                static_cast<ULONG>(input_len),
                                &auth_info,
                                nullptr,
                                0,
                                output,
                                static_cast<ULONG>(input_len),
                                &output_bytes,
                                0);
        }
        destroy_key(key);
        close_algorithm(algorithm);
        return status >= 0 && output_bytes == input_len
            ? SAO_STATUS_OK
            : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        destroy_key(key);
        close_algorithm(algorithm);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace
#endif

extern "C" sao_status_t SAO_CORE_CALL sao_core_hash(
    int32_t algo,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_digest,
    size_t digest_capacity) {
#ifdef _WIN32
    return hash_bytes(algo, nullptr, 0, data, data_len, out_digest, digest_capacity);
#else
    (void)algo;
    (void)data;
    (void)data_len;
    (void)out_digest;
    (void)digest_capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_hmac(
    int32_t algo,
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_mac,
    size_t mac_capacity) {
#ifdef _WIN32
    if (key == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return hash_bytes(algo, key, key_len, data, data_len, out_mac, mac_capacity);
#else
    (void)algo;
    (void)key;
    (void)key_len;
    (void)data;
    (void)data_len;
    (void)out_mac;
    (void)mac_capacity;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_aes_gcm_encrypt(
    const uint8_t* key32,
    const uint8_t* nonce12,
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* plaintext,
    size_t plaintext_len,
    uint8_t* out_ciphertext,
    uint8_t out_tag16[16]) {
#ifdef _WIN32
    return aes_gcm_crypt(true,
                         key32,
                         nonce12,
                         aad,
                         aad_len,
                         plaintext,
                         plaintext_len,
                         out_ciphertext,
                         out_tag16);
#else
    (void)key32;
    (void)nonce12;
    (void)aad;
    (void)aad_len;
    (void)plaintext;
    (void)plaintext_len;
    (void)out_ciphertext;
    (void)out_tag16;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_aes_gcm_decrypt(
    const uint8_t* key32,
    const uint8_t* nonce12,
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* ciphertext,
    size_t ciphertext_len,
    const uint8_t tag16[16],
    uint8_t* out_plaintext) {
#ifdef _WIN32
    return aes_gcm_crypt(false,
                         key32,
                         nonce12,
                         aad,
                         aad_len,
                         ciphertext,
                         ciphertext_len,
                         out_plaintext,
                         const_cast<uint8_t*>(tag16));
#else
    (void)key32;
    (void)nonce12;
    (void)aad;
    (void)aad_len;
    (void)ciphertext;
    (void)ciphertext_len;
    (void)tag16;
    (void)out_plaintext;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_random_bytes(
    uint8_t* out_buffer, size_t byte_count) {
#ifdef _WIN32
    if ((byte_count != 0 && out_buffer == nullptr) ||
        byte_count > std::numeric_limits<ULONG>::max()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (byte_count == 0) {
        return SAO_STATUS_OK;
    }
    return BCryptGenRandom(nullptr,
                           out_buffer,
                           static_cast<ULONG>(byte_count),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0
        ? SAO_STATUS_OK
        : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)out_buffer;
    (void)byte_count;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_dpapi_protect(
    const uint8_t* plaintext, size_t plaintext_size,
    uint8_t* out, size_t capacity, size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (plaintext == nullptr && plaintext_size > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    DATA_BLOB in_blob{};
    DATA_BLOB out_blob{};
    in_blob.cbData = static_cast<DWORD>(plaintext_size);
    in_blob.pbData = const_cast<BYTE*>(plaintext);
    if (!::CryptProtectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_LOCAL_MACHINE, &out_blob)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const size_t needed = static_cast<size_t>(out_blob.cbData);
    if (out_size != nullptr) *out_size = needed;
    sao_status_t result = SAO_STATUS_OK;
    if (out == nullptr) {
        result = capacity == 0 ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
    } else if (capacity < needed) {
        result = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    } else {
        std::memcpy(out, out_blob.pbData, needed);
    }
    ::LocalFree(out_blob.pbData);
    return result;
#else
    (void)out;
    (void)capacity;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_dpapi_unprotect(
    const uint8_t* ciphertext, size_t ciphertext_size,
    uint8_t* out, size_t capacity, size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (ciphertext == nullptr && ciphertext_size > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    DATA_BLOB in_blob{};
    DATA_BLOB out_blob{};
    in_blob.cbData = static_cast<DWORD>(ciphertext_size);
    in_blob.pbData = const_cast<BYTE*>(ciphertext);
    if (!::CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_LOCAL_MACHINE, &out_blob)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const size_t needed = static_cast<size_t>(out_blob.cbData);
    if (out_size != nullptr) *out_size = needed;
    sao_status_t result = SAO_STATUS_OK;
    if (out == nullptr) {
        result = capacity == 0 ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
    } else if (capacity < needed) {
        result = SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    } else {
        std::memcpy(out, out_blob.pbData, needed);
    }
    ::LocalFree(out_blob.pbData);
    return result;
#else
    (void)out;
    (void)capacity;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

namespace {

constexpr uint8_t kSao3Magic[4] = {'S', 'A', 'O', '3'};
constexpr uint32_t kSao3Version = 1;

inline void sao3_write_le_u32(uint8_t* buffer, uint32_t value) {
    buffer[0] = static_cast<uint8_t>(value & 0xFFu);
    buffer[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    buffer[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    buffer[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline uint32_t sao3_read_le_u32(const uint8_t* buffer) {
    return static_cast<uint32_t>(buffer[0]) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);
}

}  // namespace

// Wipes a key material buffer on scope exit, covering early returns.
struct ScopedKeyWipe {
    uint8_t* data;
    size_t size;
    ~ScopedKeyWipe() {
        volatile uint8_t* wipe = data;
        for (size_t i = 0; i < size; ++i) wipe[i] = 0;
    }
};

extern "C" bool SAO_CORE_CALL sao_core_sao3_is_envelope(
    const uint8_t* data, size_t data_size) {
    if (data == nullptr || data_size < 4u) return false;
    return std::memcmp(data, kSao3Magic, 4) == 0;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_sao3_envelope_encode(
    const uint8_t* plaintext, size_t plaintext_size,
    uint8_t* out, size_t out_capacity, size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (plaintext == nullptr && plaintext_size > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    if (plaintext_size > 0xFFFFFFFFu) {
        // pt_size on the wire is a little-endian u32.
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    uint8_t data_key[32];
    ScopedKeyWipe data_key_wipe{data_key, sizeof(data_key)};
    sao_status_t rs = sao_core_random_bytes(data_key, sizeof(data_key));
    if (rs != SAO_STATUS_OK) return rs;

    size_t dpapi_size = 0;
    sao_status_t ps = sao_core_dpapi_protect(
        data_key, sizeof(data_key), nullptr, 0, &dpapi_size);
    if (ps != SAO_STATUS_OK) return ps;
    if (dpapi_size == 0 || dpapi_size > 0x10000u) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    std::vector<uint8_t> dpapi_blob(dpapi_size);
    ps = sao_core_dpapi_protect(data_key, sizeof(data_key),
                                dpapi_blob.data(), dpapi_blob.size(),
                                &dpapi_size);
    if (ps != SAO_STATUS_OK) return ps;
    dpapi_blob.resize(dpapi_size);

    uint8_t nonce[12];
    rs = sao_core_random_bytes(nonce, sizeof(nonce));
    if (rs != SAO_STATUS_OK) return rs;

    uint8_t tag[16] = {0};
    std::vector<uint8_t> ct(plaintext_size);
    const sao_status_t es = sao_core_aes_gcm_encrypt(
        data_key, nonce, nullptr, 0,
        plaintext, plaintext_size,
        ct.empty() ? nullptr : ct.data(), tag);
    if (es != SAO_STATUS_OK) return es;

    const size_t needed = 4u + 4u + 4u + dpapi_size + 12u + 16u + 4u +
                          plaintext_size;
    if (out_size != nullptr) *out_size = needed;
    if (out == nullptr || out_capacity < needed) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    uint8_t* p = out;
    std::memcpy(p, kSao3Magic, 4); p += 4;
    sao3_write_le_u32(p, kSao3Version); p += 4;
    sao3_write_le_u32(p, static_cast<uint32_t>(dpapi_size)); p += 4;
    std::memcpy(p, dpapi_blob.data(), dpapi_size); p += dpapi_size;
    std::memcpy(p, nonce, 12); p += 12;
    std::memcpy(p, tag, 16); p += 16;
    sao3_write_le_u32(p, static_cast<uint32_t>(plaintext_size)); p += 4;
    if (plaintext_size > 0) std::memcpy(p, ct.data(), plaintext_size);
    return SAO_STATUS_OK;
#else
    (void)out;
    (void)out_capacity;
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_sao3_envelope_decode(
    const uint8_t* envelope, size_t envelope_size,
    uint8_t* pt_out, size_t pt_capacity, size_t* bytes_written) {
    if (bytes_written != nullptr) *bytes_written = 0;
    if (envelope == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!sao_core_sao3_is_envelope(envelope, envelope_size) ||
        envelope_size < 16u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (sao3_read_le_u32(envelope + 4) != kSao3Version) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint32_t blob_len = sao3_read_le_u32(envelope + 8);
    if (blob_len > 0x10000u ||
        envelope_size < 12u + blob_len + 32u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint8_t* dpapi_blob = envelope + 12;
    size_t key_size = 0;
    sao_status_t us = sao_core_dpapi_unprotect(
        dpapi_blob, blob_len, nullptr, 0, &key_size);
    if (us != SAO_STATUS_OK || key_size != 32u) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    uint8_t data_key[32];
    ScopedKeyWipe data_key_wipe{data_key, sizeof(data_key)};
    us = sao_core_dpapi_unprotect(dpapi_blob, blob_len,
                                  data_key, sizeof(data_key), &key_size);
    if (us != SAO_STATUS_OK || key_size != 32u) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    const uint8_t* rest = envelope + 12 + blob_len;
    const size_t rest_len = envelope_size - 12 - blob_len;
    // rest_len >= 32 was verified above.
    const uint8_t* nonce = rest;
    const uint8_t* tag = rest + 12;
    const uint32_t pt_size = sao3_read_le_u32(rest + 28);
    // Subtraction form avoids u32 wraparound in `32u + pt_size`.
    if (pt_size > rest_len - 32u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const uint8_t* ct = rest + 32;

    if (bytes_written != nullptr) *bytes_written = pt_size;
    if (pt_out == nullptr || pt_capacity < pt_size) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    const sao_status_t ds = sao_core_aes_gcm_decrypt(
        data_key, nonce, nullptr, 0, ct, pt_size, tag, pt_out);
    if (ds != SAO_STATUS_OK) {
        if (bytes_written != nullptr) *bytes_written = 0;
        return ds;
    }
    return SAO_STATUS_OK;
}
