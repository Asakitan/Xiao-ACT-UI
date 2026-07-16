#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/license/client/client_public.h"
#include "sao/license/sdk/license_sdk.h"
#include "sao_core/sao_status.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string appendOperation(std::string endpoint, const char* operation) {
    if (!endpoint.empty() && endpoint.back() != '/') endpoint.push_back('/');
    endpoint.append(operation ? operation : "");
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
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
        (components.nScheme != INTERNET_SCHEME_HTTPS &&
         components.nScheme != INTERNET_SCHEME_HTTP)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    HINTERNET session = WinHttpOpen(
        L"SaoAuto/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return SAO_ERR_OS_CALL_FAILED;
    HINTERNET connection = WinHttpConnect(
        session, host.c_str(), components.nPort, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return SAO_ERR_OS_CALL_FAILED;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return SAO_ERR_OS_CALL_FAILED;
    }
    const auto request_size = static_cast<DWORD>(strlen(request_json));
    BOOL succeeded = WinHttpSendRequest(
        request, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1),
        const_cast<char*>(request_json), request_size, request_size, 0);
    if (succeeded) succeeded = WinHttpReceiveResponse(request, nullptr);
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (succeeded) {
        succeeded = WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
            WINHTTP_NO_HEADER_INDEX);
    }
    std::vector<char> response;
    while (succeeded) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            succeeded = FALSE;
            break;
        }
        if (available == 0) break;
        if (response.size() + available > response_capacity) {
            succeeded = FALSE;
            break;
        }
        const auto offset = response.size();
        response.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, response.data() + offset,
                             available, &received)) {
            succeeded = FALSE;
            break;
        }
        response.resize(offset + received);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!succeeded) return SAO_ERR_OS_CALL_FAILED;
    if (!response.empty()) {
        memcpy_s(response_json, response_capacity,
                 response.data(), response.size());
    }
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
    sao_license_tier_t tier = SAO_LICENSE_TIER_UNKNOWN;
    uint64_t expiry_ms = 0;
    std::array<uint8_t, 32> hwid{};
    if (status == SAO_OK) status = sao_license_sdk_get_tier(&tier);
    if (status == SAO_OK) status = sao_license_sdk_get_expiry_ms(&expiry_ms);
    if (status == SAO_OK) status = sao_license_sdk_get_hwid(hwid.data());
    if (status != SAO_OK) {
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license verification failed", _TRUNCATE);
        return SAO_STATUS_LICENSE_INVALID;
    }
    out->valid = 1;
    out->expires_utc = static_cast<int64_t>(expiry_ms / 1000U);
    strncpy_s(out->tier, sizeof(out->tier), tierName(tier), _TRUNCATE);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < hwid.size(); ++index) {
        out->hwid_hash[index * 2] = digits[hwid[index] >> 4U];
        out->hwid_hash[index * 2 + 1] = digits[hwid[index] & 0x0fU];
    }
    if (configuration.heartbeat_interval_ms != 0 &&
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