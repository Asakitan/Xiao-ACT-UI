#include "native_secret_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "native_utils.h"

namespace sao::ai_editor::native {
namespace {

using Json = nlohmann::json;

// Shared with the Python-side ProtectedSecretStore so a vault written by
// either implementation is loadable by the other under the same user.
constexpr char kEntropy[] = "sao-ai-editor-secret-store-v1";
constexpr uint32_t kMaxProtectedBytes = 256U * 1024U;
constexpr uint32_t kMaxPlaintextBytes = 64U * 1024U;

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(unsigned char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::string base64_encode(const uint8_t* data, size_t size) {
    std::string result;
    result.reserve(((size + 2) / 3) * 4);
    for (size_t index = 0; index < size; index += 3) {
        const uint32_t byte0 = data[index];
        const bool has_byte1 = index + 1 < size;
        const bool has_byte2 = index + 2 < size;
        const uint32_t byte1 = has_byte1 ? data[index + 1] : 0;
        const uint32_t byte2 = has_byte2 ? data[index + 2] : 0;
        const uint32_t value = (byte0 << 16) | (byte1 << 8) | byte2;
        result.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        result.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        result.push_back(has_byte1 ? kBase64Alphabet[(value >> 6) & 0x3F] : '=');
        result.push_back(has_byte2 ? kBase64Alphabet[value & 0x3F] : '=');
    }
    return result;
}

bool base64_decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    if (text.size() % 4 != 0 || text.size() / 4 > (kMaxProtectedBytes / 3 + 1)) {
        return false;
    }
    out.reserve((text.size() / 4) * 3);
    for (size_t index = 0; index < text.size(); index += 4) {
        const int first = base64_value(static_cast<unsigned char>(text[index]));
        const int second =
            base64_value(static_cast<unsigned char>(text[index + 1]));
        if (first < 0 || second < 0) {
            return false;
        }
        const bool third_padding = text[index + 2] == '=';
        const bool fourth_padding = text[index + 3] == '=';
        const int third = third_padding
            ? 0
            : base64_value(static_cast<unsigned char>(text[index + 2]));
        const int fourth = fourth_padding
            ? 0
            : base64_value(static_cast<unsigned char>(text[index + 3]));
        if ((!third_padding && third < 0) ||
            (!fourth_padding && fourth < 0) ||
            (third_padding && !fourth_padding)) {
            return false;
        }
        const uint32_t value =
            (static_cast<uint32_t>(first) << 18) |
            (static_cast<uint32_t>(second) << 12) |
            (static_cast<uint32_t>(third) << 6) | static_cast<uint32_t>(fourth);
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        if (!third_padding) {
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        }
        if (!fourth_padding) {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
        }
    }
    return true;
}

struct LocalBuffer {
    DATA_BLOB blob{};
    ~LocalBuffer() {
        if (blob.pbData != nullptr) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
            blob.pbData = nullptr;
            blob.cbData = 0;
        }
    }
};

// Global lock: the vault is a single file shared across all SecretStore
// instances in this process.  We serialize reads and writes together so
// concurrent providers.configure invocations cannot clobber each other.
std::mutex& vault_lock() {
    static std::mutex mutex;
    return mutex;
}

int32_t dpapi_protect(std::string_view plaintext, std::string& base64_out) {
    if (plaintext.size() > kMaxPlaintextBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE*>(
                        const_cast<char*>(plaintext.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBuffer output;
    if (!::CryptProtectData(&input, L"SAO AI Editor", &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (output.blob.pbData == nullptr || output.blob.cbData == 0 ||
        output.blob.cbData > kMaxProtectedBytes) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    base64_out = base64_encode(output.blob.pbData, output.blob.cbData);
    return SAO_AI_EDITOR_OK;
}

int32_t dpapi_unprotect(std::string_view base64_in, std::string& plaintext) {
    std::vector<uint8_t> raw;
    if (!base64_decode(base64_in, raw) || raw.empty() ||
        raw.size() > kMaxProtectedBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    DATA_BLOB input{static_cast<DWORD>(raw.size()), raw.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBuffer output;
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (output.blob.cbData > kMaxPlaintextBytes) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    plaintext.assign(reinterpret_cast<const char*>(output.blob.pbData),
                     output.blob.cbData);
    return SAO_AI_EDITOR_OK;
}

int32_t read_vault(const std::filesystem::path& path, Json& vault) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        vault = Json{{"version", 1}, {"entries", Json::object()}};
        return SAO_AI_EDITOR_OK;
    }
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    std::string text;
    const int32_t status = read_text_file(path, kMaximumJsonBytes, text);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json parsed = Json::parse(text, nullptr, false);
    if (!parsed.is_object() || parsed.value("version", 0) != 1 ||
        !parsed.contains("entries") || !parsed["entries"].is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    vault = std::move(parsed);
    return SAO_AI_EDITOR_OK;
}

int32_t write_vault(const std::filesystem::path& path, const Json& vault) {
    return write_text_atomic(path, vault.dump(2));
}

}  // namespace

SecretStore::SecretStore(std::filesystem::path vault_path) noexcept
    : vault_path_(std::move(vault_path)) {}

bool SecretStore::valid_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 512) {
        return false;
    }
    for (const unsigned char character : key) {
        const bool alphanumeric = std::isalnum(character) != 0;
        const bool punctuation = character == '.' || character == '_' ||
                                 character == '/' || character == '@' ||
                                 character == ':' || character == '-';
        if (!alphanumeric && !punctuation) {
            return false;
        }
    }
    size_t cursor = 0;
    while (cursor <= key.size()) {
        const size_t next = key.find('/', cursor);
        const std::string_view part = key.substr(
            cursor, next == std::string_view::npos ? std::string_view::npos
                                                   : next - cursor);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (next == std::string_view::npos) {
            break;
        }
        cursor = next + 1;
    }
    return true;
}

int32_t SecretStore::set(std::string_view key, std::string_view value) {
    if (!valid_key(key)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::string protected_text;
    const int32_t protected_status = dpapi_protect(value, protected_text);
    if (protected_status != SAO_AI_EDITOR_OK) {
        return protected_status;
    }
    std::lock_guard<std::mutex> lock(vault_lock());
    Json vault;
    const int32_t read_status = read_vault(vault_path_, vault);
    if (read_status != SAO_AI_EDITOR_OK) {
        return read_status;
    }
    vault["entries"][std::string(key)] = std::move(protected_text);
    return write_vault(vault_path_, vault);
}

int32_t SecretStore::get(std::string_view key, std::string& value) const {
    value.clear();
    if (!valid_key(key)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(vault_lock());
    Json vault;
    const int32_t read_status = read_vault(vault_path_, vault);
    if (read_status != SAO_AI_EDITOR_OK) {
        return read_status;
    }
    const auto entries = vault["entries"];
    const std::string owned_key(key);
    if (!entries.contains(owned_key)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const auto& entry = entries[owned_key];
    if (!entry.is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return dpapi_unprotect(entry.get<std::string>(), value);
}

int32_t SecretStore::erase(std::string_view key) {
    if (!valid_key(key)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(vault_lock());
    Json vault;
    const int32_t read_status = read_vault(vault_path_, vault);
    if (read_status != SAO_AI_EDITOR_OK) {
        return read_status;
    }
    const std::string owned_key(key);
    if (!vault["entries"].contains(owned_key)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    vault["entries"].erase(owned_key);
    return write_vault(vault_path_, vault);
}

bool SecretStore::has(std::string_view key) const {
    if (!valid_key(key)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(vault_lock());
    Json vault;
    if (read_vault(vault_path_, vault) != SAO_AI_EDITOR_OK) {
        return false;
    }
    return vault["entries"].contains(std::string(key));
}

}  // namespace sao::ai_editor::native
