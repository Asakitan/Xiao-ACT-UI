// SAO Auto -- runtime installer download engine.
//
// A thin blocking wrapper over WinHTTP that streams the response body to a
// caller-provided sink.  We deliberately reimplement rather than reuse the
// sao_net_http_get() surface because:
//
//   1. Runtime payloads reach 100 MiB; sao_net_http_get() reads the full
//      body into a heap buffer and reports it back to callers who then
//      write to disk.  For runtime downloads we want incremental IO with a
//      user-controlled sink so we can pipe bytes straight through the hash
//      contexts + zip staging file without doubling memory pressure.
//   2. sao_net_http_get() cannot expose the byte-stream to progress
//      callbacks with monotonic accuracy (buffered writer breaks the model).
//   3. Test hook injection is easier when the transport is a local
//      function pointer and not a live TCP client.
//
// URLs are validated with a tiny scheme+host scanner; only https:// is
// accepted in production builds (test hook overrides run entirely offline).

#include "download_engine_internal.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "sao_security/obfuscation/enc_str.h"

namespace sao::runtime_installer::internal {

namespace {

// User-Agent lives behind the ENC_STR shroud so plaintext "SAO Auto..."
// never sits in .rdata.  WinHTTP still needs a wide string, so we widen
// once on demand.
std::wstring build_user_agent() {
    const auto ua = SAO_ENC_STR("SaoAutoRuntimeInstaller/1.0");
    const char* raw = ua.decrypt();
    const size_t len = std::strlen(raw);
    std::wstring out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(raw[i])));
    }
    return out;
}

struct HttpsUrl {
    std::wstring host;
    INTERNET_PORT port = 443;
    std::wstring path;   // includes leading '/'
};

bool parse_https_url(const char* url_utf8, HttpsUrl& out) {
    if (url_utf8 == nullptr) return false;
    // Reject anything but https:// -- the manifest already vetted URLs but
    // defence in depth is cheap.
    const auto scheme = SAO_ENC_STR("https://");
    const char* scheme_plain = scheme.decrypt();
    const size_t scheme_len = std::strlen(scheme_plain);
    if (std::strncmp(url_utf8, scheme_plain, scheme_len) != 0) return false;
    const char* host_begin = url_utf8 + scheme_len;
    const char* p = host_begin;
    while (*p != '\0' && *p != '/' && *p != ':') ++p;
    if (p == host_begin) return false;
    std::wstring host;
    host.reserve(static_cast<size_t>(p - host_begin));
    for (const char* q = host_begin; q < p; ++q) {
        const unsigned char c = static_cast<unsigned char>(*q);
        // Host chars are ASCII per manifest constraints; anything else is
        // a hard reject.
        if (c > 0x7f) return false;
        host.push_back(static_cast<wchar_t>(c));
    }
    INTERNET_PORT port = 443;
    if (*p == ':') {
        ++p;
        int v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            ++p;
            ++digits;
            if (digits > 5 || v > 65535) return false;
        }
        if (digits == 0) return false;
        port = static_cast<INTERNET_PORT>(v);
    }
    std::wstring path;
    if (*p == '\0') {
        path = L"/";
    } else {
        path.reserve(std::strlen(p));
        for (; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c > 0x7f) return false;
            path.push_back(static_cast<wchar_t>(c));
        }
    }
    out.host = std::move(host);
    out.port = port;
    out.path = std::move(path);
    return true;
}

struct WinHttpDeleter {
    void operator()(HINTERNET h) const noexcept {
        if (h != nullptr) ::WinHttpCloseHandle(h);
    }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

}  // namespace

sao_status_t stream_download(const char* url_utf8,
                             DownloadSink& sink,
                             uint64_t max_bytes,
                             sao_runtime_installer_progress_cb_t progress_cb,
                             void* user_data) noexcept {
    if (url_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    HttpsUrl parsed{};
    if (!parse_https_url(url_utf8, parsed)) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    const std::wstring ua = build_user_agent();
    WinHttpHandle session(::WinHttpOpen(
        ua.c_str(),
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) return SAO_STATUS_ERR_OS_CALL_FAILED;

    // 60s timeouts across the board; large downloads use the sink loop for
    // additional read-timeout tolerance.
    const int timeout_ms = 60'000;
    if (!::WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms)) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    WinHttpHandle connection(::WinHttpConnect(
        session.get(), parsed.host.c_str(), parsed.port, 0));
    if (!connection) return SAO_STATUS_ERR_NET_DOWN;

    WinHttpHandle request(::WinHttpOpenRequest(
        connection.get(),
        L"GET",
        parsed.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) return SAO_STATUS_ERR_OS_CALL_FAILED;

    if (!::WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        return SAO_STATUS_ERR_NET_DOWN;
    }
    if (!::WinHttpReceiveResponse(request.get(), nullptr)) {
        return SAO_STATUS_ERR_NET_DOWN;
    }

    // HTTP status must be 200 -- redirects are handled transparently by
    // WinHttp with the default flags, so anything else here is fatal.
    DWORD status_code = 0;
    DWORD status_len = sizeof(status_code);
    if (!::WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_len,
            WINHTTP_NO_HEADER_INDEX)) {
        return SAO_STATUS_ERR_NET_HTTP_STATUS;
    }
    if (status_code != 200) return SAO_STATUS_ERR_NET_HTTP_STATUS;

    // Content-Length is advisory -- servers may omit it for chunked
    // transfers.  Zero total means "unknown" for the progress callback.
    uint64_t declared_total = 0;
    {
        wchar_t buf[32];
        DWORD buf_len = sizeof(buf);
        if (::WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buf,
                &buf_len,
                WINHTTP_NO_HEADER_INDEX)) {
            uint64_t v = 0;
            for (DWORD i = 0; i < buf_len / sizeof(wchar_t); ++i) {
                const wchar_t c = buf[i];
                if (c == 0) break;
                if (c < L'0' || c > L'9') { v = 0; break; }
                v = v * 10ull + static_cast<uint64_t>(c - L'0');
            }
            declared_total = v;
        }
    }
    if (declared_total > max_bytes) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    std::vector<uint8_t> chunk(64 * 1024);
    uint64_t received = 0;
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
            return SAO_STATUS_ERR_NET_DOWN;
        }
        if (available == 0) break;
        const DWORD take = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
        DWORD read_bytes = 0;
        if (!::WinHttpReadData(request.get(), chunk.data(), take, &read_bytes)) {
            return SAO_STATUS_ERR_NET_DOWN;
        }
        if (read_bytes == 0) break;
        received += read_bytes;
        if (received > max_bytes) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (const auto s = sink.write(chunk.data(), read_bytes); s != SAO_STATUS_OK) {
            return s;
        }
        if (progress_cb != nullptr) {
            progress_cb(received, declared_total, user_data);
        }
    }
    sink.commit_length(received);
    return SAO_STATUS_OK;
}

}  // namespace sao::runtime_installer::internal
