#include "sao/net/tls.h"

#include "winhttp_internal.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <map>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

struct sao_net_tls_pin_set_s {
    std::vector<std::array<uint8_t, 32>> pins;
};

namespace {

constexpr size_t kMaximumCertificateBytes = 4 * 1024 * 1024;
constexpr size_t kMaximumScopeBytes = 64;

std::mutex g_installed_mutex;
std::map<std::string, std::vector<std::array<uint8_t, 32>>> g_installed_pins;

bool valid_scope(const char* scope) noexcept {
    if (scope == nullptr) return false;
    const size_t length = ::strnlen(scope, kMaximumScopeBytes + 1);
    if (length == 0 || length > kMaximumScopeBytes) return false;
    return std::strcmp(scope, SAO_NET_TLS_SCOPE_LICENSE) == 0 ||
           std::strcmp(scope, SAO_NET_TLS_SCOPE_UPDATE) == 0 ||
           std::strcmp(scope, SAO_NET_TLS_SCOPE_WORKSHOP) == 0 ||
           std::strcmp(scope, SAO_NET_TLS_SCOPE_CLOUD) == 0 ||
           std::strcmp(scope, SAO_NET_TLS_SCOPE_DEFAULT) == 0;
}

bool constant_time_equal(const uint8_t left[32], const uint8_t right[32]) noexcept {
    uint8_t difference = 0;
    for (size_t index = 0; index < 32; ++index) {
        difference = static_cast<uint8_t>(difference | (left[index] ^ right[index]));
    }
    return difference == 0;
}

sao_status_t sha256(const uint8_t* bytes, size_t length,
                    uint8_t digest_out[32]) noexcept {
    std::memset(digest_out, 0, 32);
    if (bytes == nullptr || length == 0 || length > static_cast<size_t>(ULONG_MAX)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    }
    if (status >= 0) {
        status = BCryptHashData(hash, const_cast<PUCHAR>(bytes),
                                static_cast<ULONG>(length), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest_out, 32, 0);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        SecureZeroMemory(digest_out, 32);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

sao_status_t certificate_spki_sha256(PCCERT_CONTEXT certificate,
                                     uint8_t digest_out[32]) noexcept {
    std::memset(digest_out, 0, 32);
    if (certificate == nullptr || certificate->pCertInfo == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    BYTE* encoded = nullptr;
    DWORD encoded_length = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             &certificate->pCertInfo->SubjectPublicKeyInfo,
                             CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             &encoded, &encoded_length) ||
        encoded == nullptr || encoded_length == 0) {
        if (encoded != nullptr) LocalFree(encoded);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const auto status = sha256(encoded, encoded_length, digest_out);
    SecureZeroMemory(encoded, encoded_length);
    LocalFree(encoded);
    return status;
}

bool pins_match(const std::vector<std::array<uint8_t, 32>>& pins,
                const uint8_t digest[32]) noexcept {
    bool matched = false;
    for (const auto& pin : pins) {
        matched = constant_time_equal(pin.data(), digest) || matched;
    }
    return matched;
}

std::vector<std::array<uint8_t, 32>> installed_pin_snapshot(
    const char* scope_utf8) {
    std::vector<std::array<uint8_t, 32>> pins;
    std::lock_guard<std::mutex> guard(g_installed_mutex);
    const auto found = g_installed_pins.find(scope_utf8);
    if (found != g_installed_pins.end()) pins = found->second;
    return pins;
}

bool has_installed_pins(const char* scope_utf8) noexcept {
    if (!valid_scope(scope_utf8)) return false;
    std::lock_guard<std::mutex> guard(g_installed_mutex);
    const auto found = g_installed_pins.find(scope_utf8);
    return found != g_installed_pins.end() && !found->second.empty();
}

bool verify_server_certificate(PCCERT_CONTEXT certificate,
                               const wchar_t* server_name) noexcept {
    if (certificate == nullptr || certificate->pCertInfo == nullptr ||
        server_name == nullptr || server_name[0] == L'\0') {
        return false;
    }
    if (CertVerifyTimeValidity(nullptr, certificate->pCertInfo) != 0) {
        return false;
    }

    CERT_CHAIN_PARA chain_parameters{};
    chain_parameters.cbSize = sizeof(chain_parameters);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(nullptr, certificate, nullptr,
                                 certificate->hCertStore, &chain_parameters,
                                 0, nullptr, &chain) || chain == nullptr) {
        return false;
    }

    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_parameters{};
    ssl_parameters.cbStruct = sizeof(ssl_parameters);
    ssl_parameters.dwAuthType = AUTHTYPE_SERVER;
    ssl_parameters.pwszServerName = const_cast<wchar_t*>(server_name);

    CERT_CHAIN_POLICY_PARA policy_parameters{};
    policy_parameters.cbSize = sizeof(policy_parameters);
    policy_parameters.pvExtraPolicyPara = &ssl_parameters;
    CERT_CHAIN_POLICY_STATUS policy_status{};
    policy_status.cbSize = sizeof(policy_status);
    const bool policy_called = CertVerifyCertificateChainPolicy(
        CERT_CHAIN_POLICY_SSL, chain, &policy_parameters, &policy_status) != FALSE;
    const DWORD policy_error = policy_status.dwError;
    CertFreeCertificateChain(chain);
    if (!policy_called) return false;

    return policy_error == ERROR_SUCCESS || policy_error == CERT_E_UNTRUSTEDROOT;
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_tls_pin_set_create(
    sao_net_tls_pin_set_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    try {
        *out_handle = new sao_net_tls_pin_set_s();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_tls_pin_set_add_sha256(
    sao_net_tls_pin_set_handle_t handle, const uint8_t digest[32]) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (digest == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::array<uint8_t, 32> value{};
    std::memcpy(value.data(), digest, value.size());
    if (std::find(handle->pins.begin(), handle->pins.end(), value) != handle->pins.end()) {
        return SAO_STATUS_OK;
    }
    try {
        handle->pins.push_back(value);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_NET_CALL sao_net_tls_pin_set_destroy(
    sao_net_tls_pin_set_handle_t handle) {
    if (handle == nullptr) return;
    for (auto& pin : handle->pins) SecureZeroMemory(pin.data(), pin.size());
    delete handle;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_tls_install_pin_set(
    const char* scope_utf8, sao_net_tls_pin_set_handle_t handle) {
    if (!valid_scope(scope_utf8)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->pins.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> guard(g_installed_mutex);
        g_installed_pins[scope_utf8] = handle->pins;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_tls_certificate_spki_sha256(
    const uint8_t* certificate_der, size_t certificate_der_length,
    uint8_t digest_out[32]) {
    if (digest_out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::memset(digest_out, 0, 32);
    if (certificate_der == nullptr || certificate_der_length == 0 ||
        certificate_der_length > kMaximumCertificateBytes ||
        certificate_der_length >
            static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    PCCERT_CONTEXT certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING, certificate_der,
        static_cast<DWORD>(certificate_der_length));
    if (certificate == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto status = certificate_spki_sha256(certificate, digest_out);
    CertFreeCertificateContext(certificate);
    return status;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_tls_validate_certificate(
    sao_net_tls_pin_set_handle_t handle, const uint8_t* certificate_der,
    size_t certificate_der_length) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->pins.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    uint8_t digest[32]{};
    const auto status = sao_net_tls_certificate_spki_sha256(
        certificate_der, certificate_der_length, digest);
    if (status != SAO_STATUS_OK) return status;
    const bool matched = pins_match(handle->pins, digest);
    SecureZeroMemory(digest, sizeof(digest));
    return matched ? SAO_STATUS_OK : SAO_STATUS_ERR_NET_TLS;
}

namespace sao::net::internal {

sao_status_t validate_request_scope(const char* scope_utf8) noexcept {
    return valid_scope(scope_utf8) ? SAO_STATUS_OK
                                   : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

sao_status_t configure_pinned_security(HINTERNET request,
                                       const char* scope_utf8) noexcept {
    const auto scope_status = validate_request_scope(scope_utf8);
    if (scope_status != SAO_STATUS_OK) return scope_status;
    if (!has_installed_pins(scope_utf8)) return SAO_STATUS_OK;
    if (request == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    DWORD security_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS,
                          &security_flags, sizeof(security_flags))) {
        return map_winhttp_error(GetLastError());
    }
    return SAO_STATUS_OK;
}

sao_status_t validate_request_certificate(HINTERNET request,
                                          const wchar_t* server_name,
                                          const char* scope_utf8) noexcept {
    const auto scope_status = validate_request_scope(scope_utf8);
    if (scope_status != SAO_STATUS_OK) return scope_status;
    auto pins = installed_pin_snapshot(scope_utf8);
    if (pins.empty()) return SAO_STATUS_OK;
    if (request == nullptr || server_name == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    PCCERT_CONTEXT certificate = nullptr;
    DWORD size = sizeof(certificate);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT,
                            &certificate, &size) || certificate == nullptr) {
        return SAO_STATUS_ERR_NET_TLS;
    }
    if (!verify_server_certificate(certificate, server_name)) {
        CertFreeCertificateContext(certificate);
        for (auto& pin : pins) SecureZeroMemory(pin.data(), pin.size());
        return SAO_STATUS_ERR_NET_TLS;
    }
    uint8_t digest[32]{};
    const auto status = certificate_spki_sha256(certificate, digest);
    CertFreeCertificateContext(certificate);
    if (status != SAO_STATUS_OK) return status;
    const bool matched = pins_match(pins, digest);
    SecureZeroMemory(digest, sizeof(digest));
    for (auto& pin : pins) SecureZeroMemory(pin.data(), pin.size());
    return matched ? SAO_STATUS_OK : SAO_STATUS_ERR_NET_TLS;
}

}  // namespace sao::net::internal
