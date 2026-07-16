#include "winhttp_internal.h"

#include "sao/net/url.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

namespace sao::net::internal {

sao_status_t utf8_to_wide(const char* input, size_t maximum_bytes,
                          std::wstring& output) noexcept {
    output.clear();
    if (input == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const size_t length = ::strnlen(input, maximum_bytes + 1);
    if (length > maximum_bytes || length > static_cast<size_t>(INT_MAX)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (length == 0) return SAO_STATUS_OK;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input, static_cast<int>(length), nullptr, 0);
    if (required <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        output.resize(static_cast<size_t>(required));
    } catch (...) {
        output.clear();
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input,
                            static_cast<int>(length), output.data(), required) != required) {
        output.clear();
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

sao_status_t wide_to_utf8(const wchar_t* input, size_t input_length,
                          std::string& output) noexcept {
    output.clear();
    if (input == nullptr || input_length > static_cast<size_t>(INT_MAX)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (input_length == 0) return SAO_STATUS_OK;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input, static_cast<int>(input_length),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        output.resize(static_cast<size_t>(required));
    } catch (...) {
        output.clear();
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input,
                            static_cast<int>(input_length), output.data(), required,
                            nullptr, nullptr) != required) {
        output.clear();
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

sao_status_t parse_winhttp_url(const char* url_utf8, bool websocket,
                               ParsedWinHttpUrl& output) noexcept {
    output = {};
    SaoUrlParts parts{};
    const auto parse_status = sao_net_url_parse(url_utf8, &parts);
    if (parse_status != SAO_STATUS_OK) return parse_status;

    const std::string_view url(url_utf8);
    const auto scheme = url.substr(parts.scheme_off, parts.scheme_len);
    const bool secure = scheme == "https" || scheme == "wss";
    const bool cleartext = scheme == "http" || scheme == "ws";
    if (!secure && !cleartext) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (websocket != (scheme == "ws" || scheme == "wss")) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::string host(url.substr(parts.host_off, parts.host_len));
    auto status = utf8_to_wide(host.c_str(), 253, output.host);
    if (status != SAO_STATUS_OK || output.host.empty()) {
        output = {};
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const size_t path_end = parts.path_off + parts.path_len;
    size_t suffix_end = url.size();
    if (parts.fragment_off != 0 || parts.fragment_len != 0 ||
        (!url.empty() && url.back() == '#')) {
        suffix_end = static_cast<size_t>(parts.fragment_off) - 1;
    }
    const size_t request_begin = parts.path_len == 0 ? path_end : parts.path_off;
    std::string request_target;
    try {
        if (parts.path_len == 0) request_target = "/";
        else request_target.assign(url.substr(request_begin, suffix_end - request_begin));
        if (parts.path_len == 0 && parts.query_off != 0) {
            request_target.push_back('?');
            request_target.append(url.substr(parts.query_off, parts.query_len));
        }
    } catch (...) {
        output = {};
        return SAO_STATUS_ERR_UNKNOWN;
    }
    status = utf8_to_wide(request_target.c_str(), kMaximumUrlBytes, output.path);
    if (status != SAO_STATUS_OK || output.path.empty()) {
        output = {};
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    output.secure = secure;
    output.websocket = websocket;
    output.port = parts.port_present
                      ? parts.port
                      : static_cast<INTERNET_PORT>(secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                          : INTERNET_DEFAULT_HTTP_PORT);
    return SAO_STATUS_OK;
}

sao_status_t normalize_headers(const char* headers_utf8,
                               std::wstring& output) noexcept {
    output.clear();
    if (headers_utf8 == nullptr || headers_utf8[0] == '\0') return SAO_STATUS_OK;
    const size_t length = ::strnlen(headers_utf8, kMaximumHeadersBytes + 1);
    if (length == 0 || length > kMaximumHeadersBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::string_view headers(headers_utf8, length);
    size_t line_begin = 0;
    while (line_begin < headers.size()) {
        const size_t line_end = headers.find("\r\n", line_begin);
        const size_t limit = line_end == std::string_view::npos ? headers.size() : line_end;
        const auto line = headers.substr(line_begin, limit - line_begin);
        if (line.empty()) {
            if (limit + 2 != headers.size()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            break;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        for (const char value : line.substr(0, colon)) {
            const auto byte = static_cast<unsigned char>(value);
            const bool token = std::isalnum(byte) != 0 || value == '!' || value == '#' ||
                               value == '$' || value == '%' || value == '&' || value == '\'' ||
                               value == '*' || value == '+' || value == '-' || value == '.' ||
                               value == '^' || value == '_' || value == '`' || value == '|' ||
                               value == '~';
            if (!token) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (line.find('\0') != std::string_view::npos || line.find('\n') != std::string_view::npos ||
            line.find('\r') != std::string_view::npos) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (line_end == std::string_view::npos) break;
        line_begin = line_end + 2;
    }
    auto status = utf8_to_wide(headers_utf8, kMaximumHeadersBytes, output);
    if (status != SAO_STATUS_OK) output.clear();
    return status;
}

sao_status_t map_winhttp_error(DWORD error) noexcept {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return SAO_STATUS_ERR_TIMEOUT;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_RESEND_REQUEST:
        return SAO_STATUS_ERR_NET_DOWN;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return SAO_STATUS_ERR_NET_TLS;
    case ERROR_ACCESS_DENIED:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    default:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

uint32_t effective_timeout(uint32_t timeout_ms) noexcept {
    return timeout_ms == 0 ? kDefaultTimeoutMs : timeout_ms;
}

sao_status_t send_request_and_receive(
    WinHttpHandle& request, const uint8_t* body, size_t body_length,
    uint32_t timeout_ms) noexcept {
    if (!request || body_length > static_cast<size_t>(std::numeric_limits<DWORD>::max()) ||
        (body_length != 0 && body == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completed == nullptr) return SAO_STATUS_ERR_OS_CALL_FAILED;

    const HINTERNET raw_request = request.get();
    std::atomic<DWORD> operation_error{ERROR_SUCCESS};
    std::atomic<bool> operation_ok{false};
    try {
        std::thread worker([&] {
            LPVOID request_body = body_length == 0
                                       ? WINHTTP_NO_REQUEST_DATA
                                       : const_cast<uint8_t*>(body);
            bool succeeded = WinHttpSendRequest(
                                 raw_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 request_body, static_cast<DWORD>(body_length),
                                 static_cast<DWORD>(body_length), 0) != FALSE;
            if (succeeded) {
                succeeded = WinHttpReceiveResponse(raw_request, nullptr) != FALSE;
            }
            if (!succeeded) operation_error.store(GetLastError(), std::memory_order_release);
            operation_ok.store(succeeded, std::memory_order_release);
            SetEvent(completed);
        });

        const DWORD wait = WaitForSingleObject(completed, effective_timeout(timeout_ms));
        if (wait == WAIT_TIMEOUT) {
            const HINTERNET cancelled = request.release();
            if (cancelled != nullptr) WinHttpCloseHandle(cancelled);
            worker.join();
            CloseHandle(completed);
            return SAO_STATUS_ERR_TIMEOUT;
        }
        if (wait != WAIT_OBJECT_0) {
            const HINTERNET cancelled = request.release();
            if (cancelled != nullptr) WinHttpCloseHandle(cancelled);
            worker.join();
            CloseHandle(completed);
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        worker.join();
    } catch (...) {
        CloseHandle(completed);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    CloseHandle(completed);
    return operation_ok.load(std::memory_order_acquire)
               ? SAO_STATUS_OK
               : map_winhttp_error(operation_error.load(std::memory_order_acquire));
}

}  // namespace sao::net::internal
