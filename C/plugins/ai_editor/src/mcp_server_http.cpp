// Streamable HTTP MCP transport for the AI Editor.

#include "mcp_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "native_utils.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace sao::ai_editor {
namespace {

using Json = nlohmann::json;

constexpr uint16_t kDefaultPort = 9820;
constexpr size_t kMaximumHeaderBytes = 64U * 1024U;
constexpr size_t kMaximumBodyBytes = 4U * 1024U * 1024U;
constexpr uint32_t kSocketTimeoutMs = 10000U;

#if defined(_WIN32)

class SocketHandle final {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET value) noexcept : value_(value) {}
    ~SocketHandle() { reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, INVALID_SOCKET));
        }
        return *this;
    }
    void reset(SOCKET value = INVALID_SOCKET) noexcept {
        if (value_ != INVALID_SOCKET) {
            ::shutdown(value_, SD_BOTH);
            ::closesocket(value_);
        }
        value_ = value;
    }
    SOCKET get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_SOCKET; }
private:
    SOCKET value_ = INVALID_SOCKET;
};

struct HttpServerState final {
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    std::atomic<bool> running{false};
    SOCKET listener = INVALID_SOCKET;
    uint16_t port = kDefaultPort;
    std::string token;
    std::thread listener_thread;
    std::vector<std::thread> client_threads;
    std::unordered_set<SOCKET> active_clients;
    sao_ai_editor_runtime_t runtime = nullptr;
    std::string workspace;
    bool wsa_started = false;
};

HttpServerState g_server;

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(
            static_cast<unsigned char>(character)));
    }
    return value;
}

std::string trim_ascii(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool send_all(SOCKET client, const char* data, size_t size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(size, 64U * 1024U));
        const int sent = ::send(client, data, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void set_socket_timeouts(SOCKET client) {
    const DWORD timeout = kSocketTimeoutMs;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void send_http_response(SOCKET client, int status, std::string_view reason,
                        std::string_view body) {
    const std::string header =
        "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
        "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n";
    (void)send_all(client, header.data(), header.size());
    (void)send_all(client, body.data(), body.size());
}

void send_http_no_content(SOCKET client, int status,
                          std::string_view reason) {
    const std::string header =
        "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    (void)send_all(client, header.data(), header.size());
}

struct HttpRequest final {
    std::string target;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

bool read_request(SOCKET client, HttpRequest& request, int& status) {
    std::string wire;
    std::array<char, 4096> buffer{};
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int received = ::recv(client, buffer.data(),
                                    static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            status = 400;
            return false;
        }
        wire.append(buffer.data(), static_cast<size_t>(received));
        if (wire.size() > kMaximumHeaderBytes) {
            status = 431;
            return false;
        }
        header_end = wire.find("\r\n\r\n");
    }

    const std::string header_block = wire.substr(0, header_end);
    const size_t request_line_end = header_block.find("\r\n");
    if (request_line_end == std::string::npos) {
        status = 400;
        return false;
    }
    std::istringstream request_line(header_block.substr(0, request_line_end));
    std::string method;
    std::string version;
    if (!(request_line >> method >> request.target >> version) ||
        method != "POST" || version != "HTTP/1.1") {
        status = method == "POST" ? 400 : 405;
        return false;
    }
    std::string extra;
    if (request_line >> extra || request.target.size() > 8192U) {
        status = 400;
        return false;
    }

    size_t line_begin = request_line_end + 2U;
    while (line_begin < header_block.size()) {
        const size_t line_end = header_block.find("\r\n", line_begin);
        const size_t end = line_end == std::string::npos
            ? header_block.size() : line_end;
        const std::string_view line(header_block.data() + line_begin,
                                    end - line_begin);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            status = 400;
            return false;
        }
        const std::string name = lower_ascii(trim_ascii(line.substr(0, colon)));
        const std::string value = trim_ascii(line.substr(colon + 1U));
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos ||
            request.headers.find(name) != request.headers.end()) {
            status = 400;
            return false;
        }
        request.headers.emplace(name, value);
        if (line_end == std::string::npos) {
            break;
        }
        line_begin = line_end + 2U;
    }

    const auto host = request.headers.find("host");
    const auto content_type = request.headers.find("content-type");
    const auto content_length = request.headers.find("content-length");
    if (host == request.headers.end() || content_type == request.headers.end() ||
        content_length == request.headers.end() || host->second.empty() ||
        lower_ascii(content_type->second) != "application/json" ||
        request.headers.find("transfer-encoding") != request.headers.end()) {
        status = content_type == request.headers.end() ? 415 : 400;
        return false;
    }
    const std::string length_text = content_length->second;
    if (length_text.empty() ||
        !std::all_of(length_text.begin(), length_text.end(),
                     [](unsigned char value) { return std::isdigit(value) != 0; })) {
        status = 400;
        return false;
    }
    uint64_t length = 0;
    try {
        length = std::stoull(length_text);
    } catch (...) {
        status = 400;
        return false;
    }
    if (length > kMaximumBodyBytes) {
        status = 413;
        return false;
    }
    const size_t body_begin = header_end + 4U;
    request.body = wire.substr(body_begin);
    while (request.body.size() < length) {
        const int received = ::recv(client, buffer.data(),
                                    static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            status = 400;
            return false;
        }
        request.body.append(buffer.data(), static_cast<size_t>(received));
        if (request.body.size() > length) {
            request.body.resize(static_cast<size_t>(length));
            break;
        }
    }
    request.body.resize(static_cast<size_t>(length));
    return true;
}

bool valid_host(std::string_view host, uint16_t port) {
    const std::string lower = lower_ascii(std::string(host));
    return lower == "localhost" || lower == "127.0.0.1" ||
           lower == "localhost:" + std::to_string(port) ||
           lower == "127.0.0.1:" + std::to_string(port);
}

bool authenticated(const HttpRequest& request, uint16_t port,
                   std::string_view token) {
    const auto host = request.headers.find("host");
    if (host == request.headers.end() || !valid_host(host->second, port)) {
        return false;
    }
    const auto authorization = request.headers.find("authorization");
    return authorization != request.headers.end() &&
           authorization->second == "Bearer " + std::string(token);
}

Json jsonrpc_error(const Json& id, int code, std::string_view message) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", code},
                            {"message", std::string(message)}}}};
}

void client_thread(SOCKET client) {
    SocketHandle owned(client);
    HttpRequest request;
    int status = 400;
    if (!read_request(client, request, status)) {
        send_http_response(client, status,
                           status == 405 ? "Method Not Allowed" :
                           status == 413 ? "Payload Too Large" :
                           status == 415 ? "Unsupported Media Type" :
                           status == 431 ? "Request Header Fields Too Large" :
                                           "Bad Request",
                           "{\"error\":\"invalid HTTP request\"}");
    } else {
        const std::string path = request.target.substr(0, request.target.find('?'));
        std::string token;
        uint16_t port = 0;
        sao_ai_editor_runtime_t runtime = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_server.mutex);
            token = g_server.token;
            port = g_server.port;
            runtime = g_server.runtime;
        }
        if (path != "/mcp") {
            send_http_response(client, 404, "Not Found", "{\"error\":\"not found\"}");
        } else if (!authenticated(request, port, token)) {
            send_http_response(client, 401, "Unauthorized", "{\"error\":\"authentication required\"}");
        } else {
            const Json parsed = Json::parse(request.body, nullptr, false);
            if (parsed.is_discarded()) {
                send_http_response(client, 200, "OK",
                                   jsonrpc_error(nullptr, -32700,
                                                 "parse error").dump());
            } else if (!parsed.is_object() ||
                       parsed.value("jsonrpc", "") != "2.0" ||
                       !parsed.contains("method") ||
                       !parsed["method"].is_string()) {
                const Json id = parsed.is_object()
                    ? parsed.value("id", Json(nullptr)) : Json(nullptr);
                send_http_response(client, 200, "OK",
                                   jsonrpc_error(id, -32600,
                                                 "invalid JSON-RPC request").dump());
            } else {
                Json response;
                const int32_t rc = runtime == nullptr
                    ? SAO_AI_EDITOR_ERR_HANDLE_INVALID
                    : sao::ai_editor::native::dispatch_mcp_message(runtime, parsed, response);
                if (rc != SAO_AI_EDITOR_OK) {
                    send_http_response(client, 200, "OK",
                                       jsonrpc_error(
                                           parsed.value("id", Json(nullptr)),
                                           -32000, "dispatch failed").dump());
                } else if (response.is_null()) {
                    send_http_no_content(client, 202, "Accepted");
                } else {
                    const std::string body = response.dump();
                    if (body.size() > kMaximumBodyBytes) {
                        send_http_response(client, 413, "Payload Too Large", "{\"error\":\"response too large\"}");
                    } else {
                        send_http_response(client, 200, "OK", body);
                    }
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_server.mutex);
        g_server.active_clients.erase(client);
    }
}

void listener_loop() {
    for (;;) {
        SOCKET listener = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> guard(g_server.mutex);
            if (!g_server.running.load(std::memory_order_acquire)) {
                break;
            }
            listener = g_server.listener;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener, &read_set);
        timeval timeout{0, 200000};
        const int selected = ::select(0, &read_set, nullptr, nullptr, &timeout);
        if (selected <= 0) {
            continue;
        }
        SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        set_socket_timeouts(client);
        std::lock_guard<std::mutex> guard(g_server.mutex);
        if (!g_server.running.load(std::memory_order_acquire)) {
            ::closesocket(client);
            break;
        }
        g_server.active_clients.insert(client);
        g_server.client_threads.emplace_back(client_thread, client);
    }
}

std::string generate_token() {
    std::array<uint8_t, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2U);
    for (const uint8_t byte : bytes) {
        token.push_back(hex[byte >> 4U]);
        token.push_back(hex[byte & 0x0FU]);
    }
    return token;
}

int start_server(uint16_t port, std::string workspace_utf8,
                 std::string token) {
    std::unique_lock<std::mutex> lifecycle(g_server.lifecycle_mutex);
    if (port == 0) port = kDefaultPort;
    if (token.empty() || token.size() > 256U ||
        token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") != std::string::npos) {
        return -1;
    }
    if (workspace_utf8.empty() || !sao::ai_editor::native::valid_utf8(workspace_utf8)) {
        return -1;
    }
    const std::filesystem::path requested(sao::ai_editor::native::utf8_to_wide(workspace_utf8));
    std::error_code error;
    const std::filesystem::path workspace = std::filesystem::weakly_canonical(requested, error);
    if (error || !std::filesystem::is_directory(workspace, error) || error) {
        return -1;
    }
    workspace_utf8 = sao::ai_editor::native::wide_to_utf8(workspace.native());
    if (workspace_utf8.empty()) return -1;
    std::lock_guard<std::mutex> guard(g_server.mutex);
    if (g_server.running.load(std::memory_order_acquire) ||
        g_server.listener_thread.joinable() ||
        !g_server.client_threads.empty() || g_server.runtime != nullptr) {
        return -1;
    }
    SaoAiEditorRuntimeConfig config{};
    config.struct_size = sizeof(config);
    config.workspace_root_utf8 = workspace_utf8.c_str();
    sao_ai_editor_runtime_t runtime = nullptr;
    if (sao_ai_editor_runtime_create(&config, &runtime) != SAO_AI_EDITOR_OK || runtime == nullptr) {
        return -1;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        sao_ai_editor_runtime_destroy(runtime);
        return -1;
    }
    g_server.wsa_started = true;
    SocketHandle listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener) {
        WSACleanup();
        g_server.wsa_started = false;
        sao_ai_editor_runtime_destroy(runtime);
        return -1;
    }
    const BOOL reuse = TRUE;
    ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 16) != 0) {
        WSACleanup();
        g_server.wsa_started = false;
        sao_ai_editor_runtime_destroy(runtime);
        return -1;
    }
    g_server.listener = listener.get();
    listener.reset();
    g_server.port = port;
    g_server.token = std::move(token);
    g_server.workspace = std::move(workspace_utf8);
    g_server.runtime = runtime;
    g_server.running.store(true, std::memory_order_release);
    g_server.listener_thread = std::thread(listener_loop);
    return 0;
}

#endif

}  // namespace

extern "C" int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http(uint16_t port) {
#if defined(_WIN32)
    const std::string workspace = sao::ai_editor::native::wide_to_utf8(
        std::filesystem::current_path().native());
    const std::string token = generate_token();
    return token.empty() ? -1 : start_server(port, workspace, token);
#else
    (void)port;
    return -1;
#endif
}

extern "C" int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_with_token(
    uint16_t port, const char* token_utf8) {
#if defined(_WIN32)
    if (token_utf8 == nullptr) return -1;
    const std::string workspace = sao::ai_editor::native::wide_to_utf8(
        std::filesystem::current_path().native());
    return start_server(port, workspace, token_utf8);
#else
    (void)port;
    (void)token_utf8;
    return -1;
#endif
}

extern "C" int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_ex(
    uint16_t port, const char* workspace_utf8, const char* token_utf8) {
#if defined(_WIN32)
    if (workspace_utf8 == nullptr || token_utf8 == nullptr) return -1;
    return start_server(port, workspace_utf8, token_utf8);
#else
    (void)port;
    (void)workspace_utf8;
    (void)token_utf8;
    return -1;
#endif
}

extern "C" int SAO_AI_EDITOR_CALL
sao_ai_editor_stop_mcp_server_streamable_http(void) {
#if defined(_WIN32)
    std::unique_lock<std::mutex> lifecycle(g_server.lifecycle_mutex);
    sao_ai_editor_runtime_t runtime = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_server.mutex);
        if (!g_server.running.exchange(false, std::memory_order_acq_rel)) {
            g_server.port = 0;
            return 0;
        }
        const SOCKET listener = g_server.listener;
        g_server.listener = INVALID_SOCKET;
        runtime = g_server.runtime;
        g_server.runtime = nullptr;
        if (listener != INVALID_SOCKET) {
            ::shutdown(listener, SD_BOTH);
            ::closesocket(listener);
        }
        for (const SOCKET client : g_server.active_clients) ::shutdown(client, SD_BOTH);
    }
    if (g_server.listener_thread.joinable()) g_server.listener_thread.join();
    std::vector<std::thread> clients;
    {
        std::lock_guard<std::mutex> guard(g_server.mutex);
        clients.swap(g_server.client_threads);
    }
    for (auto& client : clients) if (client.joinable()) client.join();
    {
        std::lock_guard<std::mutex> guard(g_server.mutex);
        g_server.active_clients.clear();
        g_server.token.clear();
        g_server.workspace.clear();
        g_server.port = 0;
        if (g_server.wsa_started) {
            WSACleanup();
            g_server.wsa_started = false;
        }
    }
    if (runtime != nullptr) sao_ai_editor_runtime_destroy(runtime);
    return 0;
#else
    return -1;
#endif
}

extern "C" uint16_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_http_port(void) {
#if defined(_WIN32)
    std::lock_guard<std::mutex> guard(g_server.mutex);
    return g_server.port;
#else
    return 0;
#endif
}

extern "C" int SAO_AI_EDITOR_CALL sao_ai_editor_mcp_http_token(
    char* token_out, uint32_t token_capacity, uint32_t* token_size) {
#if defined(_WIN32)
    std::string token;
    {
        std::lock_guard<std::mutex> guard(g_server.mutex);
        token = g_server.token;
    }
    if (token.empty()) return SAO_AI_EDITOR_ERR_NOT_FOUND;
    return sao::ai_editor::native::copy_text_to_caller(
        token, token_out, token_capacity, token_size);
#else
    (void)token_out;
    (void)token_capacity;
    (void)token_size;
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
#endif
}

}  // namespace sao::ai_editor
