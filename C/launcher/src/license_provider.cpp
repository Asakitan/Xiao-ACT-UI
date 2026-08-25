#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/license/client/client_public.h"
#include "sao/license/sdk/license_sdk.h"
#include "sao/license/sdk/license_status.h"
#include "sao_core/sao_status.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;
constexpr size_t kMaximumLicenseRequestBytes = 1024u * 1024u;

using WinHttpOpenFn = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
using WinHttpConnectFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
using WinHttpOpenRequestFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR,
                                                LPCWSTR, LPCWSTR*, DWORD);
using WinHttpSendRequestFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD,
                                           DWORD_PTR);
using WinHttpWriteDataFn = BOOL(WINAPI*)(HINTERNET, LPCVOID, DWORD, LPDWORD);
using WinHttpReceiveResponseFn = BOOL(WINAPI*)(HINTERNET, LPVOID);
using WinHttpQueryHeadersFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD,
                                            LPDWORD);
using WinHttpQueryDataAvailableFn = BOOL(WINAPI*)(HINTERNET, LPDWORD);
using WinHttpReadDataFn = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);
using WinHttpSetTimeoutsFn = BOOL(WINAPI*)(HINTERNET, int, int, int, int);
using WinHttpSetOptionFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);
using WinHttpQueryOptionFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, LPDWORD);
using WinHttpCloseHandleFn = BOOL(WINAPI*)(HINTERNET);
using WinHttpCrackUrlFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
using GetLastErrorFn = DWORD(WINAPI*)();

struct WinHttpApi final {
    WinHttpOpenFn open;
    WinHttpConnectFn connect;
    WinHttpOpenRequestFn open_request;
    WinHttpSendRequestFn send_request;
    WinHttpWriteDataFn write_data;
    WinHttpReceiveResponseFn receive_response;
    WinHttpQueryHeadersFn query_headers;
    WinHttpQueryDataAvailableFn query_data_available;
    WinHttpReadDataFn read_data;
    WinHttpSetTimeoutsFn set_timeouts;
    WinHttpSetOptionFn set_option;
    WinHttpQueryOptionFn query_option;
    WinHttpCloseHandleFn close_handle;
    WinHttpCrackUrlFn crack_url;
    GetLastErrorFn get_last_error;
};

const WinHttpApi& productionWinHttpApi() noexcept {
    static const WinHttpApi api{
        &::WinHttpOpen,
        &::WinHttpConnect,
        &::WinHttpOpenRequest,
        &::WinHttpSendRequest,
        &::WinHttpWriteData,
        &::WinHttpReceiveResponse,
        &::WinHttpQueryHeaders,
        &::WinHttpQueryDataAvailable,
        &::WinHttpReadData,
        &::WinHttpSetTimeouts,
        &::WinHttpSetOption,
        &::WinHttpQueryOption,
        &::WinHttpCloseHandle,
        &::WinHttpCrackUrl,
        &::GetLastError,
    };
    return api;
}

#if defined(SAO_LICENSE_PROVIDER_TESTING)
const WinHttpApi* g_testWinHttpApi = nullptr;
#endif

const WinHttpApi& winHttpApi() noexcept {
#if defined(SAO_LICENSE_PROVIDER_TESTING)
    if (g_testWinHttpApi != nullptr) return *g_testWinHttpApi;
#endif
    return productionWinHttpApi();
}

SteadyClock::time_point productionNow() noexcept {
    return SteadyClock::now();
}

#if defined(SAO_LICENSE_PROVIDER_TESTING)
using NowFn = SteadyClock::time_point (*)();
NowFn g_testNow = &productionNow;
#endif

SteadyClock::time_point now() noexcept {
#if defined(SAO_LICENSE_PROVIDER_TESTING)
    return g_testNow();
#else
    return productionNow();
#endif
}

class InternetHandle final {
  public:
    explicit InternetHandle(const WinHttpApi& api) noexcept : api_(&api) {}
    ~InternetHandle() { reset(); }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, nullptr)) {}

    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        api_ = other.api_;
        handle_ = std::exchange(other.handle_, nullptr);
        return *this;
    }

    void reset(HINTERNET replacement = nullptr) noexcept {
        if (handle_ == replacement) return;
        HINTERNET previous = std::exchange(handle_, replacement);
        if (previous != nullptr) {
            (void)api_->close_handle(previous);
        }
    }

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    const WinHttpApi* api_;
    HINTERNET handle_ = nullptr;
};

enum class BlockingCallResult : uint8_t {
    success,
    failed,
    timed_out,
};

std::optional<DWORD> remainingTimeoutMs(SteadyClock::time_point deadline) noexcept {
    const auto remaining = deadline - now();
    if (remaining <= SteadyClock::duration::zero()) return std::nullopt;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (milliseconds <= 0) milliseconds = 1;
    const auto bounded = std::min<std::int64_t>(
        milliseconds, static_cast<std::int64_t>(std::numeric_limits<int>::max()));
    return static_cast<DWORD>(bounded);
}

BlockingCallResult prepareBlockingCall(const WinHttpApi& api, HINTERNET handle,
                                       SteadyClock::time_point deadline) noexcept {
    const auto timeout = remainingTimeoutMs(deadline);
    if (!timeout.has_value()) return BlockingCallResult::timed_out;
    if (!api.set_timeouts(handle, static_cast<int>(*timeout), static_cast<int>(*timeout),
                          static_cast<int>(*timeout), static_cast<int>(*timeout))) {
        return now() >= deadline ? BlockingCallResult::timed_out
                                  : BlockingCallResult::failed;
    }
    return now() >= deadline ? BlockingCallResult::timed_out : BlockingCallResult::success;
}

template <typename Operation>
BlockingCallResult callBlocking(const WinHttpApi& api, HINTERNET handle,
                                SteadyClock::time_point deadline, Operation&& operation) noexcept {
    const auto prepared = prepareBlockingCall(api, handle, deadline);
    if (prepared != BlockingCallResult::success) return prepared;
    const bool succeeded = operation();
    if (now() >= deadline) return BlockingCallResult::timed_out;
    return succeeded ? BlockingCallResult::success : BlockingCallResult::failed;
}

bool tls13Unsupported(DWORD error) noexcept {
    return error == ERROR_WINHTTP_INVALID_OPTION || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_PARAMETER;
}

struct LocalAllocBuffer final {
    void* value = nullptr;
    ~LocalAllocBuffer() {
        if (value != nullptr) LocalFree(value);
    }
};

struct BcryptAlgorithm final {
    BCRYPT_ALG_HANDLE value = nullptr;
    ~BcryptAlgorithm() {
        if (value != nullptr) BCryptCloseAlgorithmProvider(value, 0);
    }
};

struct BcryptHash final {
    BCRYPT_HASH_HANDLE value = nullptr;
    ~BcryptHash() {
        if (value != nullptr) BCryptDestroyHash(value);
    }
};

struct CertificateContext final {
    PCCERT_CONTEXT value = nullptr;
    ~CertificateContext() {
        if (value != nullptr) CertFreeCertificateContext(value);
    }
};

bool isAllZeroPin(const std::array<uint8_t, 32>& value) noexcept {
    uint8_t aggregate = 0;
    for (const uint8_t byte : value) aggregate |= byte;
    return aggregate == 0;
}

bool constantTimeEqual(const std::array<uint8_t, 32>& lhs,
                       const std::array<uint8_t, 32>& rhs) noexcept {
    volatile uint8_t difference = 0;
    for (size_t index = 0; index < lhs.size(); ++index) {
        difference |= static_cast<uint8_t>(lhs[index] ^ rhs[index]);
    }
    return difference == 0;
}

bool hashSubjectPublicKeyInfo(PCCERT_CONTEXT certificate,
                              std::array<uint8_t, 32>& digest) noexcept {
    if (certificate == nullptr || certificate->pCertInfo == nullptr) return false;

    BYTE* encoded_value = nullptr;
    DWORD encoded_size = 0;
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            X509_PUBLIC_KEY_INFO,
            &certificate->pCertInfo->SubjectPublicKeyInfo,
            CRYPT_ENCODE_ALLOC_FLAG,
            nullptr,
            &encoded_value,
            &encoded_size) ||
        encoded_value == nullptr || encoded_size == 0) {
        return false;
    }
    LocalAllocBuffer encoded{encoded_value};

    BcryptAlgorithm algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    BcryptHash hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(
            hash.value, encoded_value, encoded_size, 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(
            hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        return false;
    }
    return true;
}

bool validateServerCertificate(const WinHttpApi& api,
                              HINTERNET request,
                              const std::array<uint8_t, 32>& expected_pin,
                              SteadyClock::time_point deadline) noexcept {
    CertificateContext certificate;
    DWORD certificate_size = sizeof(certificate.value);
    const auto query_result = callBlocking(
        api, request, deadline, [&] {
            return api.query_option(request,
                                    WINHTTP_OPTION_SERVER_CERT_CONTEXT,
                                    &certificate.value,
                                    &certificate_size) != FALSE;
        });
    if (query_result != BlockingCallResult::success ||
        certificate.value == nullptr) {
        return false;
    }
    std::array<uint8_t, 32> actual_pin{};
    return hashSubjectPublicKeyInfo(certificate.value, actual_pin) &&
        constantTimeEqual(actual_pin, expected_pin);
}

BlockingCallResult configureSecureProtocols(const WinHttpApi& api, HINTERNET session,
                                            SteadyClock::time_point deadline) noexcept {
#if !defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2) || \
    !defined(WINHTTP_OPTION_SECURE_PROTOCOLS)
    (void)api;
    (void)session;
    (void)deadline;
    return BlockingCallResult::failed;
#else
    DWORD tls12 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    DWORD protocols = tls12;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (api.set_option(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                       sizeof(protocols))) {
        return now() >= deadline ? BlockingCallResult::timed_out
                                  : BlockingCallResult::success;
    }
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    if (tls13Unsupported(api.get_last_error()) &&
        api.set_option(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls12, sizeof(tls12))) {
        return now() >= deadline ? BlockingCallResult::timed_out
                                  : BlockingCallResult::success;
    }
#endif
    return now() >= deadline ? BlockingCallResult::timed_out : BlockingCallResult::failed;
#endif
}

std::string appendOperation(std::string endpoint, const char* operation) {
    const auto suffix_position = endpoint.find_first_of("?#");
    const auto insertion = suffix_position == std::string::npos
        ? endpoint.size()
        : suffix_position;
    std::string suffix = operation == nullptr ? std::string{} : std::string(operation);
    if (insertion != 0 && endpoint[insertion - 1] != '/') suffix.insert(0, 1, '/');
    endpoint.insert(insertion, suffix);
    return endpoint;
}

int32_t provider(const char* operation,
                 sao_license_client_provider_config_t* out,
                 void*) {
    if (operation == nullptr || out == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
    const auto configuration =
        sao::launcher::launcherProviderConfigurationSnapshot().license;
    if (!configuration.enabled || configuration.endpoint.empty()) {
        return SAO_ERR_NOT_INITIALIZED;
    }
    *out = {};
    const auto endpoint = appendOperation(configuration.endpoint, operation);
    if (endpoint.size() >= sizeof(out->endpoint) ||
        configuration.build_id.size() >= sizeof(out->build_id)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    memcpy_s(out->endpoint, sizeof(out->endpoint),
             endpoint.c_str(), endpoint.size() + 1);
    memcpy_s(out->build_id, sizeof(out->build_id),
             configuration.build_id.c_str(), configuration.build_id.size() + 1);
    std::copy(configuration.server_public_key.begin(),
              configuration.server_public_key.end(),
              out->server_ed25519_pubkey);
    out->responses_prevalidated = configuration.responses_prevalidated ? 1U : 0U;
    return SAO_OK;
    } catch (...) {
        if (out != nullptr) *out = {};
        return SAO_ERR_UNKNOWN;
    }
}

int32_t httpTransport(const char* endpoint,
                      const char* request_json,
                      char* response_json,
                      uint32_t response_capacity,
                      uint32_t* response_size_out,
                      int32_t* http_status_out,
                      void*) {
    if (endpoint == nullptr || request_json == nullptr || response_json == nullptr ||
        response_size_out == nullptr || http_status_out == nullptr ||
        response_capacity == 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
    *response_size_out = 0;
    *http_status_out = 0;
    const auto configuration =
        sao::launcher::launcherProviderConfigurationSnapshot().license;
    if (!configuration.enabled || configuration.endpoint.empty() ||
        isAllZeroPin(configuration.server_tls_spki_sha256)) {
        return SAO_ERR_NOT_INITIALIZED;
    }
    if (std::string_view(endpoint).find('#') != std::string_view::npos) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const size_t request_length =
        strnlen_s(request_json, kMaximumLicenseRequestBytes + 1u);
    if (request_length == 0u || request_length > kMaximumLicenseRequestBytes) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto request_size = static_cast<DWORD>(request_length);
    const auto deadline = now() + std::chrono::seconds(12);
    const auto& api = winHttpApi();
    const int wide_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, endpoint, -1, nullptr, 0);
    if (wide_size <= 0) return SAO_ERR_INVALID_ARGUMENT;
    std::wstring url(static_cast<size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint, -1,
                            url.data(), wide_size) <= 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    DWORD crack_flags = 0;
#if defined(ICU_REJECT_USERPWD)
    crack_flags |= ICU_REJECT_USERPWD;
#endif
    if (!api.crack_url(url.c_str(), 0, crack_flags, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0 ||
        (components.dwExtraInfoLength != 0 &&
         std::wstring_view(components.lpszExtraInfo, components.dwExtraInfoLength)
                 .find(L'#') != std::wstring_view::npos)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path = components.dwUrlPathLength == 0
        ? std::wstring{}
        : std::wstring(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    if (now() >= deadline) return SAO_ERR_OS_CALL_FAILED;
    InternetHandle session(api);
    session.reset(api.open(
        L"SaoAuto/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return SAO_ERR_OS_CALL_FAILED;
    if (configureSecureProtocols(api, session.get(), deadline) !=
        BlockingCallResult::success) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    InternetHandle connection(api);
    if (callBlocking(api, session.get(), deadline, [&] {
            connection.reset(api.connect(session.get(), host.c_str(), components.nPort, 0));
            return connection.get() != nullptr;
        }) != BlockingCallResult::success) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    DWORD request_flags = WINHTTP_FLAG_SECURE;
#if defined(WINHTTP_FLAG_NO_AUTO_REDIRECT)
    request_flags |= WINHTTP_FLAG_NO_AUTO_REDIRECT;
#endif
    InternetHandle request(api);
    request.reset(api.open_request(connection.get(), L"POST", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   request_flags));
    if (!request) return SAO_ERR_OS_CALL_FAILED;
#if defined(WINHTTP_OPTION_REDIRECT_POLICY)
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!api.set_option(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                        sizeof(redirect_policy))) {
#if !defined(WINHTTP_FLAG_NO_AUTO_REDIRECT)
        return SAO_ERR_OS_CALL_FAILED;
#endif
    }
#elif !defined(WINHTTP_FLAG_NO_AUTO_REDIRECT)
    return SAO_ERR_OS_CALL_FAILED;
#endif

    DWORD security_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
    if (!api.set_option(request.get(), WINHTTP_OPTION_SECURITY_FLAGS,
                        &security_flags, sizeof(security_flags))) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    if (callBlocking(api, request.get(), deadline, [&] {
            return api.send_request(request.get(), L"Content-Type: application/json\r\n",
                                    static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
                                    0, request_size, 0) != FALSE;
        }) != BlockingCallResult::success) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (!validateServerCertificate(api, request.get(),
                                   configuration.server_tls_spki_sha256,
                                   deadline)) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    DWORD request_bytes_written = 0;
    if (callBlocking(api, request.get(), deadline, [&] {
            return api.write_data(request.get(), request_json, request_size,
                                  &request_bytes_written) != FALSE;
        }) != BlockingCallResult::success ||
        request_bytes_written != request_size) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (callBlocking(api, request.get(), deadline,
                     [&] { return api.receive_response(request.get(), nullptr) != FALSE; }) !=
        BlockingCallResult::success) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (callBlocking(api, request.get(), deadline, [&] {
            return api.query_headers(request.get(),
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                                     WINHTTP_NO_HEADER_INDEX) != FALSE;
        }) != BlockingCallResult::success) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    std::vector<char> response;
    for (;;) {
        DWORD available = 0;
        if (callBlocking(api, request.get(), deadline,
                         [&] { return api.query_data_available(request.get(), &available) != FALSE; }) !=
            BlockingCallResult::success) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (now() >= deadline) return SAO_ERR_OS_CALL_FAILED;
        if (available == 0) break;
        if (response.size() > response_capacity ||
            available > response_capacity - response.size()) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        const auto offset = response.size();
        response.resize(offset + available);
        DWORD received = 0;
        if (callBlocking(api, request.get(), deadline, [&] {
                return api.read_data(request.get(), response.data() + offset, available,
                                     &received) != FALSE;
            }) != BlockingCallResult::success) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (now() >= deadline || received > available) return SAO_ERR_OS_CALL_FAILED;
        response.resize(offset + received);
        if (received == 0) break;
    }

    if (now() >= deadline) return SAO_ERR_OS_CALL_FAILED;
    if (!response.empty()) {
        if (now() >= deadline) return SAO_ERR_OS_CALL_FAILED;
        if (memcpy_s(response_json, response_capacity,
                    response.data(), response.size()) != 0) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (now() >= deadline) return SAO_ERR_OS_CALL_FAILED;
    *response_size_out = static_cast<uint32_t>(response.size());
    *http_status_out = static_cast<int32_t>(status_code);
    return SAO_OK;
    } catch (...) {
        *response_size_out = 0;
        *http_status_out = 0;
        return SAO_ERR_UNKNOWN;
    }
}

const char* tierName(sao_license_tier_t tier) {
    switch (tier) {
        case SAO_LICENSE_TIER_FREE: return "free";
        case SAO_LICENSE_TIER_PAID: return "pro";
        case SAO_LICENSE_TIER_INTERNAL: return "team";
        default: return "";
    }
}

#if defined(SAO_LICENSE_PROVIDER_TESTING)
void setTestWinHttpApi(const WinHttpApi* api) noexcept {
    g_testWinHttpApi = api;
}

void setTestNow(NowFn fn) noexcept {
    g_testNow = fn == nullptr ? &productionNow : fn;
}
#endif

} // namespace

extern "C" sao_status_t sao_license_verify(sao_license_result* out) {
    if (out == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    *out = {};
    try {
    const auto configuration =
        sao::launcher::launcherProviderConfigurationSnapshot().license;
    if (!configuration.enabled || configuration.endpoint.empty()) {
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license endpoint unavailable", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
    sao_license_client_set_provider(&provider, nullptr);
    sao_license_client_set_http_transport(&httpTransport, nullptr);
    int32_t status = sao_license_sdk_init();
    if (status == SAO_OK) status = sao_license_sdk_refresh();
    if (status == SAO_LICENSE_ERR_NO_TOKEN) status = SAO_OK;
    sao_license_tier_t tier = SAO_LICENSE_TIER_UNKNOWN;
    uint64_t expiry_ms = 0;
    std::array<uint8_t, 32> hwid{};
    if (status == SAO_OK) status = sao_license_sdk_get_tier(&tier);
    if (status == SAO_OK) status = sao_license_sdk_get_expiry_ms(&expiry_ms);
    if (status == SAO_OK) status = sao_license_sdk_get_hwid(hwid.data());
    if (status != SAO_OK) {
        if (status == SAO_LICENSE_ERR_HWID_MISMATCH) {
            strncpy_s(out->error_msg, sizeof(out->error_msg),
                      "hardware identity changed; reactivation required",
                      _TRUNCATE);
            return SAO_STATUS_LICENSE_HWID_MISMATCH;
        }
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license verification failed", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
    const char* tier_name = tierName(tier);
    if (tier_name[0] == '\0') {
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license tier unsupported", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
    out->valid = 1;
    out->expires_utc = static_cast<int64_t>(expiry_ms / 1000U);
    strncpy_s(out->tier, sizeof(out->tier), tier_name, _TRUNCATE);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < hwid.size(); ++index) {
        out->hwid_hash[index * 2] = digits[hwid[index] >> 4U];
        out->hwid_hash[index * 2 + 1] = digits[hwid[index] & 0x0fU];
    }
    if (tier != SAO_LICENSE_TIER_FREE &&
        configuration.heartbeat_interval_ms != 0 &&
        sao_license_client_start_heartbeat(
            configuration.heartbeat_interval_ms) != SAO_OK) {
        out->valid = 0;
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license heartbeat unavailable", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
    return SAO_STATUS_OK;
    } catch (...) {
        *out = {};
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license provider failed", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
}

extern "C" sao_status_t sao_license_shutdown(void) {
    sao_license_sdk_shutdown();
    sao_license_client_set_http_transport(nullptr, nullptr);
    sao_license_client_set_provider(nullptr, nullptr);
    return SAO_STATUS_OK;
}
