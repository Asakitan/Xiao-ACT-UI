#include "sao/core/crypto.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

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
    if (!algorithm_spec(algo, &spec) || out_digest == nullptr ||
        digest_capacity < spec.digest_size ||
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
