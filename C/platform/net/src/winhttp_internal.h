#pragma once

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "sao/core/status.h"

namespace sao::net::internal {

constexpr size_t kMaximumUrlBytes = 2048;
constexpr size_t kMaximumHeadersBytes = 64 * 1024;
constexpr size_t kMaximumBodyBytes = 64 * 1024 * 1024;
constexpr uint32_t kDefaultTimeoutMs = 30'000;

class WinHttpHandle final {
public:
    WinHttpHandle() noexcept = default;
    explicit WinHttpHandle(HINTERNET value) noexcept : value_(value) {}
    ~WinHttpHandle() {
        if (value_ != nullptr) WinHttpCloseHandle(value_);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : value_(other.release()) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HINTERNET get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    HINTERNET release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(HINTERNET value = nullptr) noexcept {
        if (value_ != nullptr) WinHttpCloseHandle(value_);
        value_ = value;
    }

private:
    HINTERNET value_ = nullptr;
};

struct ParsedWinHttpUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    bool websocket = false;
};

sao_status_t utf8_to_wide(const char* input, size_t maximum_bytes,
                          std::wstring& output) noexcept;
sao_status_t wide_to_utf8(const wchar_t* input, size_t input_length,
                          std::string& output) noexcept;
sao_status_t parse_winhttp_url(const char* url_utf8, bool websocket,
                               ParsedWinHttpUrl& output) noexcept;
sao_status_t normalize_headers(const char* headers_utf8,
                               std::wstring& output) noexcept;
sao_status_t map_winhttp_error(DWORD error) noexcept;
uint32_t effective_timeout(uint32_t timeout_ms) noexcept;
sao_status_t send_request_and_receive(
    WinHttpHandle& request,
    const uint8_t* body,
    size_t body_length,
    uint32_t timeout_ms) noexcept;

sao_status_t configure_pinned_security(HINTERNET request,
                                       const char* scope_utf8) noexcept;
sao_status_t validate_request_certificate(HINTERNET request,
                                          const wchar_t* server_name,
                                          const char* scope_utf8) noexcept;
sao_status_t validate_request_scope(const char* scope_utf8) noexcept;

}  // namespace sao::net::internal
