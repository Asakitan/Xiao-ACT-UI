#include "sao/net/http_client.h"
#include "sao/net/tls.h"

#include "winhttp_internal.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

using sao::net::internal::ParsedWinHttpUrl;
using sao::net::internal::WinHttpHandle;

struct sao_net_http_response_s {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;
    uint32_t status_code = 0;
};

namespace {

sao_status_t execute_request(const wchar_t* method, const char* url_utf8,
                             const char* scope_utf8,
                             const char* headers_utf8,
                             const char* content_type_utf8,
                             const uint8_t* body_bytes, size_t body_length,
                             uint32_t timeout_ms,
                             sao_net_http_response_handle_t* out_response,
                             bool secure_only) noexcept {
    if (out_response == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_response = nullptr;
    if (url_utf8 == nullptr || scope_utf8 == nullptr || method == nullptr ||
        body_length > sao::net::internal::kMaximumBodyBytes ||
        body_length > static_cast<size_t>(std::numeric_limits<DWORD>::max()) ||
        (body_length != 0 && body_bytes == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    auto status = sao::net::internal::validate_request_scope(scope_utf8);
    if (status != SAO_STATUS_OK) return status;

    ParsedWinHttpUrl url{};
    status = sao::net::internal::parse_winhttp_url(url_utf8, false, url);
    if (status != SAO_STATUS_OK) return status;
    if (secure_only && !url.secure) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    std::wstring headers;
    status = sao::net::internal::normalize_headers(headers_utf8, headers);
    if (status != SAO_STATUS_OK) return status;

    std::wstring content_type;
    if (content_type_utf8 != nullptr && content_type_utf8[0] != '\0') {
        status = sao::net::internal::utf8_to_wide(content_type_utf8, 256, content_type);
        if (status != SAO_STATUS_OK || content_type.find_first_of(L"\r\n") != std::wstring::npos) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    } else {
        content_type = L"application/octet-stream";
    }

    auto response = std::make_unique<sao_net_http_response_s>();
    response->session.reset(WinHttpOpen(
        L"SAO Auto Net/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!response->session) return sao::net::internal::map_winhttp_error(GetLastError());

    const auto timeout = static_cast<int>(std::min<uint32_t>(
        sao::net::internal::effective_timeout(timeout_ms), INT_MAX));
    if (!WinHttpSetTimeouts(response->session.get(), timeout, timeout, timeout, timeout)) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }

    response->connection.reset(WinHttpConnect(
        response->session.get(), url.host.c_str(), url.port, 0));
    if (!response->connection) return sao::net::internal::map_winhttp_error(GetLastError());

    const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    response->request.reset(WinHttpOpenRequest(
        response->connection.get(), method, url.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!response->request) return sao::net::internal::map_winhttp_error(GetLastError());
    if (!WinHttpSetTimeouts(response->request.get(), timeout, timeout,
                            timeout, timeout)) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }
    DWORD response_timeout = sao::net::internal::effective_timeout(timeout_ms);
    if (!WinHttpSetOption(response->request.get(),
                          WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                          &response_timeout, sizeof(response_timeout))) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }

    if (!headers.empty() && !WinHttpAddRequestHeaders(
            response->request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }
    if (body_length != 0) {
        std::wstring content_header = L"Content-Type: ";
        content_header.append(content_type);
        if (!WinHttpAddRequestHeaders(
                response->request.get(), content_header.c_str(),
                static_cast<DWORD>(content_header.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
    }

    if (url.secure) {
        status = sao::net::internal::configure_pinned_security(
            response->request.get(), scope_utf8);
        if (status != SAO_STATUS_OK) return status;
    }

    status = sao::net::internal::send_request_and_receive(
        response->request, body_bytes, body_length, timeout_ms);
    if (status != SAO_STATUS_OK) return status;
    if (url.secure) {
        status = sao::net::internal::validate_request_certificate(
            response->request.get(), url.host.c_str(), scope_utf8);
        if (status != SAO_STATUS_OK) return status;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            response->request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }
    response->status_code = status_code;
    *out_response = response.release();
    return SAO_STATUS_OK;
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_http_get_scoped_v2(
    const char* url_utf8, const char* scope_utf8, const char* headers_utf8,
    uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response) {
    return execute_request(L"GET", url_utf8, scope_utf8, headers_utf8, nullptr,
                           nullptr, 0, timeout_ms, out_response, true);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_get(
    const char* url_utf8, const char* headers_utf8, uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response) {
    return execute_request(L"GET", url_utf8, SAO_NET_TLS_SCOPE_DEFAULT,
                           headers_utf8, nullptr, nullptr, 0, timeout_ms,
                           out_response, false);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_post_scoped_v2(
    const char* url_utf8, const char* scope_utf8, const char* headers_utf8,
    const char* content_type_utf8, const uint8_t* body_bytes,
    size_t body_length, uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response) {
    return execute_request(L"POST", url_utf8, scope_utf8, headers_utf8, content_type_utf8,
                           body_bytes, body_length, timeout_ms, out_response, true);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_post(
    const char* url_utf8, const char* headers_utf8,
    const char* content_type_utf8, const uint8_t* body_bytes,
    size_t body_length, uint32_t timeout_ms,
    sao_net_http_response_handle_t* out_response) {
    return execute_request(L"POST", url_utf8, SAO_NET_TLS_SCOPE_DEFAULT,
                           headers_utf8, content_type_utf8, body_bytes,
                           body_length, timeout_ms, out_response, false);
}

extern "C" void SAO_NET_CALL sao_net_http_response_close(
    sao_net_http_response_handle_t response) {
    delete response;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_response_status(
    sao_net_http_response_handle_t response, uint32_t* out_status_code) {
    if (out_status_code == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_status_code = 0;
    if (response == nullptr || !response->request) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_status_code = response->status_code;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_response_header(
    sao_net_http_response_handle_t response, const char* name_utf8,
    char* out_utf8, size_t capacity, size_t* out_bytes_needed) {
    if (out_bytes_needed == nullptr || (out_utf8 == nullptr && capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_bytes_needed = 0;
    if (out_utf8 != nullptr && capacity != 0) out_utf8[0] = '\0';
    if (response == nullptr || !response->request) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (name_utf8 == nullptr || name_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::wstring name;
    auto status = sao::net::internal::utf8_to_wide(name_utf8, 256, name);
    if (status != SAO_STATUS_OK || name.find_first_of(L"\r\n:") != std::wstring::npos) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    DWORD wide_bytes = 0;
    WinHttpQueryHeaders(response->request.get(), WINHTTP_QUERY_CUSTOM,
                        name.c_str(), nullptr, &wide_bytes, WINHTTP_NO_HEADER_INDEX);
    const DWORD query_error = GetLastError();
    if (query_error == ERROR_WINHTTP_HEADER_NOT_FOUND) return SAO_STATUS_ERR_NOT_FOUND;
    if (query_error != ERROR_INSUFFICIENT_BUFFER || wide_bytes < sizeof(wchar_t)) {
        return sao::net::internal::map_winhttp_error(query_error);
    }

    try {
        std::wstring value(wide_bytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(response->request.get(), WINHTTP_QUERY_CUSTOM,
                                 name.c_str(), value.data(), &wide_bytes,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        const size_t chars = wide_bytes / sizeof(wchar_t);
        const size_t value_chars = chars != 0 && value[chars - 1] == L'\0'
                                       ? chars - 1
                                       : chars;
        std::string utf8;
        status = sao::net::internal::wide_to_utf8(value.data(), value_chars, utf8);
        if (status != SAO_STATUS_OK) return status;
        *out_bytes_needed = utf8.size() + 1;
        if (out_utf8 == nullptr) return SAO_STATUS_OK;
        if (capacity < utf8.size() + 1) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out_utf8, utf8.c_str(), utf8.size() + 1);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_http_response_read(
    sao_net_http_response_handle_t response, uint8_t* out_buffer,
    size_t buffer_len, size_t* out_read) {
    if (out_read == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_read = 0;
    if (response == nullptr || !response->request) return SAO_STATUS_ERR_HANDLE_INVALID;
    if ((out_buffer == nullptr && buffer_len != 0) ||
        buffer_len > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (buffer_len == 0) return SAO_STATUS_OK;
    DWORD bytes_read = 0;
    if (!WinHttpReadData(response->request.get(), out_buffer,
                         static_cast<DWORD>(buffer_len), &bytes_read)) {
        return sao::net::internal::map_winhttp_error(GetLastError());
    }
    *out_read = bytes_read;
    return SAO_STATUS_OK;
}
