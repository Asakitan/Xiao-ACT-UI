#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <catch2/catch_test_macros.hpp>

#include "sao/net/http_client.h"
#include "sao/net/tls.h"
#include "sao/net/url.h"
#include "sao/net/ws_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class SocketRuntime final {
public:
    SocketRuntime() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (ready_) WSACleanup();
    }
    bool ready() const noexcept { return ready_; }

private:
    bool ready_ = false;
};

class Socket final {
public:
    Socket() noexcept = default;
    explicit Socket(SOCKET value) noexcept : value_(value) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    SOCKET get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_SOCKET; }
    SOCKET release() noexcept {
        const auto value = value_;
        value_ = INVALID_SOCKET;
        return value;
    }
    void reset(SOCKET value = INVALID_SOCKET) noexcept {
        if (value_ != INVALID_SOCKET) closesocket(value_);
        value_ = value;
    }

private:
    SOCKET value_ = INVALID_SOCKET;
};

bool send_all(SOCKET socket, const uint8_t* bytes, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const int count = send(socket,
                               reinterpret_cast<const char*>(bytes + sent),
                               static_cast<int>(length - sent), 0);
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool send_all(SOCKET socket, const std::string& text) {
    return send_all(socket, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string receive_http_request(SOCKET socket) {
    std::string request;
    std::array<char, 4096> buffer{};
    size_t content_length = 0;
    size_t header_end = std::string::npos;
    while (request.size() < 64 * 1024) {
        const int count = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) break;
        request.append(buffer.data(), static_cast<size_t>(count));
        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto marker = request.find("Content-Length:");
                if (marker != std::string::npos && marker < header_end) {
                    content_length = static_cast<size_t>(
                        std::strtoul(request.c_str() + marker + 15, nullptr, 10));
                }
            }
        }
        if (header_end != std::string::npos &&
            request.size() >= header_end + 4 + content_length) {
            break;
        }
    }
    return request;
}

std::string base64_encode(const uint8_t* bytes, size_t length) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (size_t index = 0; index < length; index += 3) {
        const size_t remaining = length - index;
        const uint32_t block =
            (static_cast<uint32_t>(bytes[index]) << 16u) |
            (remaining > 1 ? static_cast<uint32_t>(bytes[index + 1]) << 8u : 0u) |
            (remaining > 2 ? static_cast<uint32_t>(bytes[index + 2]) : 0u);
        output.push_back(alphabet[(block >> 18u) & 63u]);
        output.push_back(alphabet[(block >> 12u) & 63u]);
        output.push_back(remaining > 1 ? alphabet[(block >> 6u) & 63u] : '=');
        output.push_back(remaining > 2 ? alphabet[block & 63u] : '=');
    }
    return output;
}

std::string websocket_accept(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<uint8_t, 20> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    REQUIRE(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM,
                                         nullptr, 0) >= 0);
    REQUIRE(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0);
    REQUIRE(BCryptHashData(hash,
                           reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                           static_cast<ULONG>(input.size()), 0) >= 0);
    REQUIRE(BCryptFinishHash(hash, digest.data(),
                             static_cast<ULONG>(digest.size()), 0) >= 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return base64_encode(digest.data(), digest.size());
}

class LocalServer final {
public:
    enum class Mode { Http, SlowHttp, WebSocket };

    explicit LocalServer(Mode mode) : mode_(mode) {
        REQUIRE(runtime_.ready());
        listener_.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        REQUIRE(listener_);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0);
        REQUIRE(listen(listener_.get(), 1) == 0);
        int address_length = sizeof(address);
        REQUIRE(getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address),
                            &address_length) == 0);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    ~LocalServer() {
        {
            std::lock_guard<std::mutex> lock(slow_mutex_);
            slow_released_ = true;
        }
        slow_event_.notify_all();
        listener_.reset();
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const noexcept { return port_; }
    const std::string& request() const noexcept { return request_; }

private:
    void run() {
        Socket client(accept(listener_.get(), nullptr, nullptr));
        if (!client) return;
        request_ = receive_http_request(client.get());
        if (mode_ == Mode::SlowHttp) {
            {
                std::unique_lock<std::mutex> lock(slow_mutex_);
                if (slow_event_.wait_for(lock, std::chrono::milliseconds(3000),
                                         [this] { return slow_released_; })) {
                    return;
                }
            }
            (void)send_all(client.get(),
                           "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        if (mode_ == Mode::Http) {
            const std::string body = request_.find("POST ") == 0 ? "posted" : "fixture-body";
            const std::string response =
                "HTTP/1.1 201 Created\r\nX-Fixture: local\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            (void)send_all(client.get(), response);
            return;
        }
        run_websocket(client.get());
    }

    void run_websocket(SOCKET client) {
        const auto key_marker = request_.find("Sec-WebSocket-Key:");
        if (key_marker == std::string::npos) return;
        const size_t value_begin = request_.find_first_not_of(" \t", key_marker + 18);
        const size_t value_end = request_.find("\r\n", value_begin);
        if (value_begin == std::string::npos || value_end == std::string::npos) return;
        const std::string key = request_.substr(value_begin, value_end - value_begin);
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
            websocket_accept(key) + "\r\n\r\n";
        if (!send_all(client, response)) return;

        std::array<uint8_t, 4096> frame{};
        const int count = recv(client, reinterpret_cast<char*>(frame.data()),
                               static_cast<int>(frame.size()), 0);
        if (count < 6) return;
        const size_t payload_length = frame[1] & 0x7fu;
        if ((frame[1] & 0x80u) == 0 || payload_length > 125 ||
            static_cast<size_t>(count) < 6 + payload_length) {
            return;
        }
        std::vector<uint8_t> payload(payload_length);
        for (size_t index = 0; index < payload_length; ++index) {
            payload[index] = frame[6 + index] ^ frame[2 + (index % 4)];
        }
        std::vector<uint8_t> echo{0x81, static_cast<uint8_t>(payload.size())};
        echo.insert(echo.end(), payload.begin(), payload.end());
        if (!send_all(client, echo.data(), echo.size())) return;

        const int close_count = recv(client, reinterpret_cast<char*>(frame.data()),
                                     static_cast<int>(frame.size()), 0);
        if (close_count >= 6 && (frame[0] & 0x0fu) == 8) {
            const std::array<uint8_t, 4> close_frame{0x88, 0x02, 0x03, 0xe8};
            (void)send_all(client, close_frame.data(), close_frame.size());
        }
    }

    SocketRuntime runtime_;
    Socket listener_;
    Mode mode_;
    uint16_t port_ = 0;
    std::thread worker_;
    std::mutex slow_mutex_;
    std::condition_variable slow_event_;
    bool slow_released_ = false;
    std::string request_;
};

std::string read_all(sao_net_http_response_handle_t response) {
    std::string output;
    std::array<uint8_t, 4> buffer{};
    for (;;) {
        size_t count = 0;
        REQUIRE(sao_net_http_response_read(response, buffer.data(), buffer.size(),
                                           &count) == SAO_STATUS_OK);
        if (count == 0) break;
        output.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
    return output;
}

struct WsResult {
    std::mutex mutex;
    std::condition_variable event;
    std::string text;
};

void SAO_NET_CALL ws_callback(int32_t type, const uint8_t* payload,
                              size_t payload_length, void* user_data) {
    auto* result = static_cast<WsResult*>(user_data);
    if (type != SAO_NET_WS_MSG_TEXT) return;
    {
        std::lock_guard<std::mutex> guard(result->mutex);
        result->text.assign(reinterpret_cast<const char*>(payload), payload_length);
    }
    result->event.notify_all();
}

struct CertificateFixture {
    std::vector<uint8_t> der;

    CertificateFixture() {
        DWORD subject_length = 0;
        REQUIRE(CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost",
                               CERT_X500_NAME_STR, nullptr, nullptr,
                               &subject_length, nullptr));
        std::vector<uint8_t> subject(subject_length);
        REQUIRE(CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost",
                               CERT_X500_NAME_STR, nullptr, subject.data(),
                               &subject_length, nullptr));
        CERT_NAME_BLOB subject_blob{};
        subject_blob.pbData = subject.data();
        subject_blob.cbData = subject_length;

        const std::wstring container =
            L"SAO-Net-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        HCRYPTPROV provider = 0;
        REQUIRE(CryptAcquireContextW(&provider, container.c_str(),
                                     MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                     CRYPT_NEWKEYSET));
        HCRYPTKEY key = 0;
        REQUIRE(CryptGenKey(provider, AT_SIGNATURE, CRYPT_EXPORTABLE, &key));
        PCCERT_CONTEXT certificate = CertCreateSelfSignCertificate(
            static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(provider),
            &subject_blob, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
        REQUIRE(certificate != nullptr);
        der.assign(certificate->pbCertEncoded,
                   certificate->pbCertEncoded + certificate->cbCertEncoded);
        CertFreeCertificateContext(certificate);
        CryptDestroyKey(key);
        CryptReleaseContext(provider, 0);
        HCRYPTPROV delete_provider = 0;
        REQUIRE(CryptAcquireContextW(&delete_provider, container.c_str(),
                         MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                         CRYPT_DELETEKEYSET));
    }
};

}  // namespace

TEST_CASE("URL parse and component codecs enforce bounds", "[net][url]") {
    const char* url = "https://[::1]:8443/a%20b?q=x#frag";
    SaoUrlParts parts{};
    REQUIRE(sao_net_url_parse(url, &parts) == SAO_STATUS_OK);
    REQUIRE(std::string(url + parts.scheme_off, parts.scheme_len) == "https");
    REQUIRE(std::string(url + parts.host_off, parts.host_len) == "::1");
    REQUIRE(std::string(url + parts.path_off, parts.path_len) == "/a%20b");
    REQUIRE(std::string(url + parts.query_off, parts.query_len) == "q=x");
    REQUIRE(std::string(url + parts.fragment_off, parts.fragment_len) == "frag");
    REQUIRE(parts.port_present);
    REQUIRE(parts.port == 8443);

    REQUIRE(sao_net_url_parse("http://user@localhost/", &parts) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(parts.scheme_len == 0);
    REQUIRE(sao_net_url_parse("http://localhost:70000/", &parts) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    size_t required = 0;
    REQUIRE(sao_net_url_encode_component("a b/中", nullptr, 0, &required) ==
            SAO_STATUS_OK);
    std::vector<char> encoded(required);
    REQUIRE(sao_net_url_encode_component("a b/中", encoded.data(), encoded.size(),
                                          &required) == SAO_STATUS_OK);
    REQUIRE(std::string(encoded.data()) == "a%20b%2F%E4%B8%AD");

    std::array<char, 3> too_small{'x', 'x', 'x'};
    REQUIRE(sao_net_url_decode_component(encoded.data(), too_small.data(),
                                          too_small.size(), &required) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(too_small[0] == '\0');
    std::vector<char> decoded(required);
    REQUIRE(sao_net_url_decode_component(encoded.data(), decoded.data(), decoded.size(),
                                          &required) == SAO_STATUS_OK);
    REQUIRE(std::string(decoded.data()) == "a b/中");
    REQUIRE(sao_net_url_decode_component("bad%2", decoded.data(), decoded.size(),
                                          &required) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(decoded[0] == '\0');
}

TEST_CASE("WinHTTP GET exposes status headers and streaming body", "[net][http]") {
    LocalServer server(LocalServer::Mode::Http);
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/fixture";
    sao_net_http_response_handle_t response = reinterpret_cast<sao_net_http_response_handle_t>(1);
    REQUIRE(sao_net_http_get(url.c_str(), "X-Test: yes\r\n", 2000, &response) ==
            SAO_STATUS_OK);
    REQUIRE(response != nullptr);

    uint32_t status = 0;
    REQUIRE(sao_net_http_response_status(response, &status) == SAO_STATUS_OK);
    REQUIRE(status == 201);
    size_t required = 0;
    REQUIRE(sao_net_http_response_header(response, "X-Fixture", nullptr, 0,
                                          &required) == SAO_STATUS_OK);
    std::vector<char> header(required);
    REQUIRE(sao_net_http_response_header(response, "X-Fixture", header.data(),
                                          header.size(), &required) == SAO_STATUS_OK);
    REQUIRE(std::string(header.data()) == "local");
    REQUIRE(read_all(response) == "fixture-body");
    sao_net_http_response_close(response);
}

TEST_CASE("WinHTTP POST carries caller body and headers", "[net][http]") {
    LocalServer server(LocalServer::Mode::Http);
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/post";
    const std::string body = "payload";
    sao_net_http_response_handle_t response = nullptr;
    REQUIRE(sao_net_http_post(
                url.c_str(), "X-Custom: abc\r\n", "text/plain",
                reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                2000, &response) == SAO_STATUS_OK);
    REQUIRE(read_all(response) == "posted");
    sao_net_http_response_close(response);
}

TEST_CASE("WinHTTP timeout and invalid input clear outputs", "[net][http]") {
    sao_net_http_response_handle_t response = reinterpret_cast<sao_net_http_response_handle_t>(1);
    REQUIRE(sao_net_http_get("not-a-url", nullptr, 100, &response) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);

    LocalServer server(LocalServer::Mode::SlowHttp);
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/slow";
    REQUIRE(sao_net_http_get(url.c_str(), nullptr, 1000, &response) ==
            SAO_STATUS_ERR_TIMEOUT);
    REQUIRE(response == nullptr);
}

TEST_CASE("WinHTTP WebSocket connects sends receives and closes", "[net][ws]") {
    LocalServer server(LocalServer::Mode::WebSocket);
    const std::string url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/socket";
    sao_net_ws_client_handle_t socket = nullptr;
    REQUIRE(sao_net_ws_connect(url.c_str(), "X-Handshake: local\r\n", 2000,
                               &socket) == SAO_STATUS_OK);
    REQUIRE(socket != nullptr);

    WsResult result;
    REQUIRE(sao_net_ws_start_receive(socket, ws_callback, &result) == SAO_STATUS_OK);
    const std::string payload = "hello";
    REQUIRE(sao_net_ws_send(socket, SAO_NET_WS_MSG_TEXT,
                            reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size()) == SAO_STATUS_OK);
    {
        std::unique_lock<std::mutex> lock(result.mutex);
        REQUIRE(result.event.wait_for(lock, std::chrono::seconds(2), [&] {
            return result.text == payload;
        }));
    }
    REQUIRE(sao_net_ws_close(socket, 1000) == SAO_STATUS_OK);
    sao_net_ws_destroy(socket);
}

TEST_CASE("TLS SPKI pin validation accepts match and rejects mismatch", "[net][tls]") {
    CertificateFixture certificate;
    std::array<uint8_t, 32> digest{};
    REQUIRE(sao_net_tls_certificate_spki_sha256(
                certificate.der.data(), certificate.der.size(), digest.data()) ==
            SAO_STATUS_OK);
    REQUIRE(std::any_of(digest.begin(), digest.end(), [](uint8_t value) {
        return value != 0;
    }));

    sao_net_tls_pin_set_handle_t pins = nullptr;
    REQUIRE(sao_net_tls_pin_set_create(&pins) == SAO_STATUS_OK);
    REQUIRE(sao_net_tls_pin_set_add_sha256(pins, digest.data()) == SAO_STATUS_OK);
    REQUIRE(sao_net_tls_validate_certificate(
                pins, certificate.der.data(), certificate.der.size()) ==
            SAO_STATUS_OK);

    auto wrong = digest;
    wrong[0] ^= 0xff;
    sao_net_tls_pin_set_handle_t wrong_pins = nullptr;
    REQUIRE(sao_net_tls_pin_set_create(&wrong_pins) == SAO_STATUS_OK);
    REQUIRE(sao_net_tls_pin_set_add_sha256(wrong_pins, wrong.data()) == SAO_STATUS_OK);
    REQUIRE(sao_net_tls_validate_certificate(
                wrong_pins, certificate.der.data(), certificate.der.size()) ==
            SAO_STATUS_ERR_NET_TLS);

    std::array<uint8_t, 32> cleared{};
    cleared.fill(0xa5);
    const std::array<uint8_t, 3> invalid_der{1, 2, 3};
    REQUIRE(sao_net_tls_certificate_spki_sha256(
                invalid_der.data(), invalid_der.size(), cleared.data()) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(std::all_of(cleared.begin(), cleared.end(), [](uint8_t value) {
        return value == 0;
    }));

    sao_net_tls_pin_set_destroy(wrong_pins);
    sao_net_tls_pin_set_destroy(pins);
}
