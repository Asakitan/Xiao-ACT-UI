#include "settings_codec_internal.h"
#include "settings_json_internal.h"

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace sao::launcher::settings_codec {
namespace {

constexpr std::size_t kAesKeyBytes = 32;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kMaxProtectedKeyBytes = 4U * 1024U;
constexpr std::size_t kMaxCipherBlobBytes = kMaxPlaintextBytes + kNonceBytes + kTagBytes;
constexpr char kEntropy[] = "SAO-Auto-settings-v1";
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::size_t base64_encoded_size(std::size_t byte_count) noexcept {
    return ((byte_count + 2U) / 3U) * 4U;
}

constexpr std::size_t kMaxProtectedKeyBase64Bytes = base64_encoded_size(kMaxProtectedKeyBytes);
constexpr std::size_t kMaxCipherBlobBase64Bytes = base64_encoded_size(kMaxCipherBlobBytes);

bool checked_add(std::size_t left, std::size_t right, std::size_t& out) noexcept {
    if (right > (std::numeric_limits<std::size_t>::max)() - left) {
        return false;
    }
    out = left + right;
    return true;
}

void secure_zero(void* data, std::size_t size) noexcept {
    if (data != nullptr && size != 0) {
        ::SecureZeroMemory(data, size);
    }
}

template <typename Container> void secure_zero(Container& value) noexcept {
    secure_zero(value.data(), value.size() * sizeof(typename Container::value_type));
}

class AlgorithmHandle final {
  public:
    AlgorithmHandle() noexcept = default;
    ~AlgorithmHandle() noexcept {
        if (handle_ != nullptr) {
            ::BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    BCRYPT_ALG_HANDLE* put() noexcept {
        return &handle_;
    }
    BCRYPT_ALG_HANDLE get() const noexcept {
        return handle_;
    }

  private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class KeyHandle final {
  public:
    KeyHandle() noexcept = default;
    ~KeyHandle() noexcept {
        if (handle_ != nullptr) {
            ::BCryptDestroyKey(handle_);
        }
    }

    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    BCRYPT_KEY_HANDLE* put() noexcept {
        return &handle_;
    }
    BCRYPT_KEY_HANDLE get() const noexcept {
        return handle_;
    }

  private:
    BCRYPT_KEY_HANDLE handle_ = nullptr;
};

class LocalBuffer final {
  public:
    LocalBuffer() noexcept = default;
    ~LocalBuffer() noexcept {
        reset();
    }

    LocalBuffer(const LocalBuffer&) = delete;
    LocalBuffer& operator=(const LocalBuffer&) = delete;

    DATA_BLOB* put() noexcept {
        return &blob_;
    }
    const DATA_BLOB& get() const noexcept {
        return blob_;
    }

  private:
    void reset() noexcept {
        if (blob_.pbData != nullptr) {
            secure_zero(blob_.pbData, blob_.cbData);
            ::LocalFree(blob_.pbData);
        }
        blob_ = {};
    }

    DATA_BLOB blob_{};
};

class SensitiveBytes final {
  public:
    SensitiveBytes() = default;
    explicit SensitiveBytes(std::size_t size) : bytes_(size) {}
    ~SensitiveBytes() noexcept {
        secure_zero(bytes_);
    }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;

    std::vector<std::uint8_t>& get() noexcept {
        return bytes_;
    }
    const std::vector<std::uint8_t>& get() const noexcept {
        return bytes_;
    }
    std::uint8_t* data() noexcept {
        return bytes_.data();
    }
    const std::uint8_t* data() const noexcept {
        return bytes_.data();
    }
    std::size_t size() const noexcept {
        return bytes_.size();
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class SensitiveString final {
  public:
    explicit SensitiveString(std::string value) : value_(std::move(value)) {}
    ~SensitiveString() noexcept {
        secure_zero(value_);
    }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;

    std::string& get() noexcept {
        return value_;
    }
    const std::string& get() const noexcept {
        return value_;
    }

  private:
    std::string value_;
};

sao_status_t size_to_ulong(std::size_t size, ULONG& out) noexcept {
    if (size > (std::numeric_limits<ULONG>::max)()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out = static_cast<ULONG>(size);
    return SAO_STATUS_OK;
}

sao_status_t open_aes_gcm_key(const std::array<std::uint8_t, kAesKeyBytes>& aes_key,
                              AlgorithmHandle& algorithm, KeyHandle& key) noexcept {
    NTSTATUS status =
        ::BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return SAO_STATUS_ERR_OS_CALL_FAILED;

    status =
        ::BCryptSetProperty(algorithm.get(), BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(status))
        return SAO_STATUS_ERR_OS_CALL_FAILED;

    status = ::BCryptGenerateSymmetricKey(algorithm.get(), key.put(), nullptr, 0,
                                          const_cast<PUCHAR>(aes_key.data()),
                                          static_cast<ULONG>(aes_key.size()), 0);
    return BCRYPT_SUCCESS(status) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
}

sao_status_t base64_encode(const std::uint8_t* data, std::size_t size, std::size_t max_input_size,
                           std::string& out) {
    if (size != 0 && data == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (size > max_input_size)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (size > ((std::numeric_limits<std::size_t>::max)() - 2) / 3) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const std::size_t encoded_size = ((size + 2) / 3) * 4;
    std::string encoded(encoded_size, '=');
    std::size_t input_index = 0;
    std::size_t output_index = 0;
    while (input_index + 3 <= size) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[input_index]) << 16) |
                                    (static_cast<std::uint32_t>(data[input_index + 1]) << 8) |
                                    static_cast<std::uint32_t>(data[input_index + 2]);
        encoded[output_index++] = kBase64Alphabet[(value >> 18) & 0x3f];
        encoded[output_index++] = kBase64Alphabet[(value >> 12) & 0x3f];
        encoded[output_index++] = kBase64Alphabet[(value >> 6) & 0x3f];
        encoded[output_index++] = kBase64Alphabet[value & 0x3f];
        input_index += 3;
    }

    const std::size_t remaining = size - input_index;
    if (remaining == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[input_index]) << 16;
        encoded[output_index] = kBase64Alphabet[(value >> 18) & 0x3f];
        encoded[output_index + 1] = kBase64Alphabet[(value >> 12) & 0x3f];
    } else if (remaining == 2) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[input_index]) << 16) |
                                    (static_cast<std::uint32_t>(data[input_index + 1]) << 8);
        encoded[output_index] = kBase64Alphabet[(value >> 18) & 0x3f];
        encoded[output_index + 1] = kBase64Alphabet[(value >> 12) & 0x3f];
        encoded[output_index + 2] = kBase64Alphabet[(value >> 6) & 0x3f];
    }

    out = std::move(encoded);
    return SAO_STATUS_OK;
}

int base64_value(unsigned char value) noexcept {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

sao_status_t validate_base64(std::string_view encoded) noexcept {
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const bool last = index + 4 == encoded.size();
        const int first = base64_value(static_cast<unsigned char>(encoded[index]));
        const int second = base64_value(static_cast<unsigned char>(encoded[index + 1]));
        if (first < 0 || second < 0) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        const bool third_padding = encoded[index + 2] == '=';
        const bool fourth_padding = encoded[index + 3] == '=';
        if ((!last && (third_padding || fourth_padding)) || (third_padding && !fourth_padding)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        const int third =
            third_padding ? 0 : base64_value(static_cast<unsigned char>(encoded[index + 2]));
        const int fourth =
            fourth_padding ? 0 : base64_value(static_cast<unsigned char>(encoded[index + 3]));
        if (third < 0 || fourth < 0 || (third_padding && (second & 0x0f) != 0) ||
            (fourth_padding && !third_padding && (third & 0x03) != 0)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t base64_decode(std::string_view encoded, std::size_t max_encoded_size,
                           std::size_t max_decoded_size, std::vector<std::uint8_t>& out) {
    if (encoded.size() > max_encoded_size) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (encoded.size() % 4 != 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (encoded.empty()) {
        out.clear();
        return SAO_STATUS_OK;
    }

    std::size_t padding = 0;
    if (encoded.back() == '=')
        ++padding;
    if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=')
        ++padding;
    const std::size_t decoded_size = encoded.size() / 4 * 3 - padding;
    if (decoded_size > max_decoded_size) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto validation_status = validate_base64(encoded);
    if (validation_status != SAO_STATUS_OK) {
        return validation_status;
    }

    SensitiveBytes decoded_storage(decoded_size);
    auto& decoded = decoded_storage.get();
    std::size_t output_index = 0;

    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const int first = base64_value(static_cast<unsigned char>(encoded[index]));
        const int second = base64_value(static_cast<unsigned char>(encoded[index + 1]));
        const bool third_padding = encoded[index + 2] == '=';
        const bool fourth_padding = encoded[index + 3] == '=';
        const int third =
            third_padding ? 0 : base64_value(static_cast<unsigned char>(encoded[index + 2]));
        const int fourth =
            fourth_padding ? 0 : base64_value(static_cast<unsigned char>(encoded[index + 3]));

        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 18) | (static_cast<std::uint32_t>(second) << 12) |
            (static_cast<std::uint32_t>(third) << 6) | static_cast<std::uint32_t>(fourth);
        decoded[output_index++] = static_cast<std::uint8_t>(value >> 16);
        if (!third_padding) {
            decoded[output_index++] = static_cast<std::uint8_t>(value >> 8);
        }
        if (!fourth_padding) {
            decoded[output_index++] = static_cast<std::uint8_t>(value);
        }
    }

    out = std::move(decoded);
    return SAO_STATUS_OK;
}

sao_status_t dpapi_protect(const std::array<std::uint8_t, kAesKeyBytes>& aes_key,
                           std::vector<std::uint8_t>& protected_key) {
    DATA_BLOB input{static_cast<DWORD>(aes_key.size()), const_cast<BYTE*>(aes_key.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBuffer output;
    if (!::CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            output.put())) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    const auto& blob = output.get();
    if (blob.pbData == nullptr || blob.cbData == 0 || blob.cbData > kMaxProtectedKeyBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    protected_key.assign(blob.pbData, blob.pbData + blob.cbData);
    return SAO_STATUS_OK;
}

sao_status_t dpapi_unprotect(const std::vector<std::uint8_t>& protected_key,
                             std::array<std::uint8_t, kAesKeyBytes>& aes_key) {
    ULONG protected_size = 0;
    if (protected_key.empty() || protected_key.size() > kMaxProtectedKeyBytes ||
        size_to_ulong(protected_key.size(), protected_size) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    DATA_BLOB input{protected_size, const_cast<BYTE*>(protected_key.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBuffer output;
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, output.put())) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    const auto& blob = output.get();
    if (blob.cbData != aes_key.size() || blob.pbData == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::copy_n(blob.pbData, aes_key.size(), aes_key.begin());
    return SAO_STATUS_OK;
}

sao_status_t aes_gcm_encrypt(const std::array<std::uint8_t, kAesKeyBytes>& aes_key,
                             const std::string& plaintext, SensitiveBytes& cipher_blob) {
    std::size_t prefix_size = 0;
    std::size_t blob_size = 0;
    if (plaintext.size() > kMaxPlaintextBytes ||
        !checked_add(kNonceBytes, kTagBytes, prefix_size) ||
        !checked_add(prefix_size, plaintext.size(), blob_size) || blob_size > kMaxCipherBlobBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    ULONG plaintext_size = 0;
    if (size_to_ulong(plaintext.size(), plaintext_size) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    AlgorithmHandle algorithm;
    KeyHandle key;
    auto status = open_aes_gcm_key(aes_key, algorithm, key);
    if (status != SAO_STATUS_OK)
        return status;

    auto& blob = cipher_blob.get();
    blob.resize(blob_size);
    auto* nonce = blob.data();
    auto* tag = nonce + kNonceBytes;
    auto* ciphertext = tag + kTagBytes;
    NTSTATUS crypto_status = ::BCryptGenRandom(nullptr, nonce, static_cast<ULONG>(kNonceBytes),
                                               BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(crypto_status))
        return SAO_STATUS_ERR_OS_CALL_FAILED;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(kTagBytes);

    std::uint8_t empty_input = 0;
    std::uint8_t empty_output = 0;
    auto* input = plaintext.empty() ? &empty_input
                                    : reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data()));
    auto* output = plaintext.empty() ? &empty_output : ciphertext;
    DWORD written = 0;
    crypto_status = ::BCryptEncrypt(key.get(), input, plaintext_size, &info, nullptr, 0, output,
                                    plaintext_size, &written, 0);
    if (!BCRYPT_SUCCESS(crypto_status) || written != plaintext_size) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

sao_status_t aes_gcm_decrypt(const std::array<std::uint8_t, kAesKeyBytes>& aes_key,
                             const std::vector<std::uint8_t>& cipher_blob,
                             SensitiveBytes& plaintext) {
    std::size_t prefix_size = 0;
    if (!checked_add(kNonceBytes, kTagBytes, prefix_size) || cipher_blob.size() < prefix_size ||
        cipher_blob.size() > kMaxCipherBlobBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::size_t ciphertext_size = cipher_blob.size() - prefix_size;
    if (ciphertext_size > kMaxPlaintextBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    ULONG ciphertext_size_ulong = 0;
    if (size_to_ulong(ciphertext_size, ciphertext_size_ulong) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    AlgorithmHandle algorithm;
    KeyHandle key;
    auto status = open_aes_gcm_key(aes_key, algorithm, key);
    if (status != SAO_STATUS_OK)
        return status;

    const auto* nonce = cipher_blob.data();
    const auto* tag = nonce + kNonceBytes;
    const auto* ciphertext = tag + kTagBytes;
    auto& output = plaintext.get();
    output.resize(ciphertext_size);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbTag = const_cast<PUCHAR>(tag);
    info.cbTag = static_cast<ULONG>(kTagBytes);

    std::uint8_t empty_input = 0;
    std::uint8_t empty_output = 0;
    auto* input = ciphertext_size == 0 ? &empty_input : const_cast<PUCHAR>(ciphertext);
    auto* output_bytes = ciphertext_size == 0 ? &empty_output : output.data();
    DWORD written = 0;
    const NTSTATUS crypto_status =
        ::BCryptDecrypt(key.get(), input, ciphertext_size_ulong, &info, nullptr, 0, output_bytes,
                        ciphertext_size_ulong, &written, 0);
    if (!BCRYPT_SUCCESS(crypto_status) || written != ciphertext_size_ulong) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

} // namespace

sao_status_t encode(const Json& document, std::string& out_envelope_utf8) noexcept {
    if (!document.is_object())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    std::array<std::uint8_t, kAesKeyBytes> aes_key{};
    try {
        SensitiveString plaintext(std::string{});
        auto status = settings_json::serialize_python_compatible(document, kMaxPlaintextBytes,
                                                                 plaintext.get());
        if (status != SAO_STATUS_OK)
            return status;

        const NTSTATUS random_status =
            ::BCryptGenRandom(nullptr, aes_key.data(), static_cast<ULONG>(aes_key.size()),
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(random_status)) {
            secure_zero(aes_key);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }

        SensitiveBytes cipher_blob;
        status = aes_gcm_encrypt(aes_key, plaintext.get(), cipher_blob);
        if (status != SAO_STATUS_OK) {
            secure_zero(aes_key);
            return status;
        }

        SensitiveBytes protected_key;
        status = dpapi_protect(aes_key, protected_key.get());
        secure_zero(aes_key);
        if (status != SAO_STATUS_OK)
            return status;

        std::string key_base64;
        status = base64_encode(protected_key.data(), protected_key.size(), kMaxProtectedKeyBytes,
                               key_base64);
        if (status != SAO_STATUS_OK)
            return status;
        std::string data_base64;
        status =
            base64_encode(cipher_blob.data(), cipher_blob.size(), kMaxCipherBlobBytes, data_base64);
        if (status != SAO_STATUS_OK)
            return status;

        Json envelope = Json::object();
        envelope["_enc"] = "v1";
        envelope["key"] = std::move(key_base64);
        envelope["data"] = std::move(data_base64);
        std::string encoded = envelope.dump(2, ' ', false, Json::error_handler_t::strict);
        if (encoded.size() > kMaxEnvelopeBytes) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        out_envelope_utf8 = std::move(encoded);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t decode(std::string_view raw_utf8, DecodeResult& out) noexcept {
    if (raw_utf8.size() > kMaxEnvelopeBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::array<std::uint8_t, kAesKeyBytes> aes_key{};
    try {
        Json outer;
        auto status = settings_json::parse_strict_limited(raw_utf8, outer);
        if (status != SAO_STATUS_OK)
            return status;
        if (!outer.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        const auto marker = outer.find("_enc");
        if (marker == outer.end() || !marker->is_string() ||
            marker->get_ref<const std::string&>() != "v1") {
            if (raw_utf8.size() > kMaxPlaintextBytes) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            DecodeResult result{std::move(outer), true};
            out = std::move(result);
            return SAO_STATUS_OK;
        }

        const auto key_value = outer.find("key");
        const auto data_value = outer.find("data");
        if (key_value == outer.end() || data_value == outer.end() || !key_value->is_string() ||
            !data_value->is_string()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        SensitiveBytes protected_key;
        status =
            base64_decode(key_value->get_ref<const std::string&>(), kMaxProtectedKeyBase64Bytes,
                          kMaxProtectedKeyBytes, protected_key.get());
        if (status != SAO_STATUS_OK)
            return status;
        SensitiveBytes cipher_blob;
        status = base64_decode(data_value->get_ref<const std::string&>(), kMaxCipherBlobBase64Bytes,
                               kMaxCipherBlobBytes, cipher_blob.get());
        if (status != SAO_STATUS_OK)
            return status;

        status = dpapi_unprotect(protected_key.get(), aes_key);
        if (status != SAO_STATUS_OK) {
            secure_zero(aes_key);
            return status;
        }

        SensitiveBytes plaintext;
        status = aes_gcm_decrypt(aes_key, cipher_blob.get(), plaintext);
        secure_zero(aes_key);
        if (status != SAO_STATUS_OK)
            return status;

        const std::string_view plaintext_utf8 =
            plaintext.size() == 0
                ? std::string_view{}
                : std::string_view(reinterpret_cast<const char*>(plaintext.data()),
                                   plaintext.size());
        Json document;
        status = settings_json::parse_strict_limited(plaintext_utf8, document);
        if (status != SAO_STATUS_OK)
            return status;
        if (!document.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        DecodeResult result{std::move(document), false};
        out = std::move(result);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (const nlohmann::json::exception&) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } catch (...) {
        secure_zero(aes_key);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::settings_codec
