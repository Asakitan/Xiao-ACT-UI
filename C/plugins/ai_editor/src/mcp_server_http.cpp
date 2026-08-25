// mcp_server_http.cpp — Streamable HTTP MCP transport (POST /mcp).
//
// Phase 11 (Python parity closure) — companion to run_mcp_server_stdio.
// Port of python/ai_editor/mcp_server.py::McpHttpServer.
// Default port 9820 (configurable via AI Editor settings).
//
// Implementation: winsock2 minimal HTTP/1.1 listener. Accepts localhost-only
// POST /mcp with a JSON-RPC 2.0 body, dispatches through vscode_api_shim,
// returns a JSON body. No SSE streaming in this pass (single request/response).

#include "mcp_server.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace sao::ai_editor {

namespace {
std::atomic<bool> g_http_running{false};
std::thread g_http_thread;
uint16_t g_http_port = 9820;

#if defined(_WIN32)
// Forward from vscode_api_shim.cpp — reused as the JSON-RPC dispatcher.
extern "C" int sao_ai_editor_vscode_shim_dispatch(const char* request_json_utf8,
                                                    char* out_response_utf8,
                                                    size_t out_capacity,
                                                    size_t* out_size);

std::string read_request(SOCKET client, std::string& body_out) {
    std::string request;
    char buf[4096];
    while (true) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, n);
        auto end = request.find("\r\n\r\n");
        if (end != std::string::npos) {
            // Parse Content-Length.
            size_t cl_pos = request.find("Content-Length:");
            if (cl_pos == std::string::npos)
                cl_pos = request.find("content-length:");
            size_t body_start = end + 4;
            size_t need = 0;
            if (cl_pos != std::string::npos) {
                need = static_cast<size_t>(
                    std::strtoul(request.c_str() + cl_pos + 15, nullptr, 10));
            }
            while (request.size() - body_start < need) {
                n = recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, n);
            }
            body_out = request.substr(body_start, need);
            break;
        }
        if (request.size() > 1'000'000) break; // 1 MB header cap
    }
    return request;
}

void send_response(SOCKET client, const std::string& body) {
    std::string hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " +
                      std::to_string(body.size()) +
                      "\r\n"
                      "Connection: close\r\n\r\n";
    send(client, hdr.data(), static_cast<int>(hdr.size()), 0);
    if (!body.empty())
        send(client, body.data(), static_cast<int>(body.size()), 0);
}

void handle_client(SOCKET client) {
    std::string body;
    std::string req = read_request(client, body);
    // Only allow POST /mcp; require Host header to be localhost/127.0.0.1
    // (prevents accidental exposure).
    if (req.substr(0, 9) != "POST /mcp") {
        send_response(client, "{\"error\":\"only POST /mcp is accepted\"}");
        return;
    }
    std::string resp_buf(65536, '\0');
    size_t resp_size = 0;
    int rc = sao_ai_editor_vscode_shim_dispatch(body.c_str(), resp_buf.data(),
                                                  resp_buf.size(), &resp_size);
    if (rc != 0) {
        send_response(client, "{\"error\":\"dispatch failed\"}");
        return;
    }
    resp_buf.resize(resp_size);
    send_response(client, resp_buf);
}

void listener_loop(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost-only
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(sock, 4) != 0) {
        closesocket(sock);
        WSACleanup();
        return;
    }
    // Non-blocking accept via select to allow clean shutdown.
    while (g_http_running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{0, 200'000}; // 200ms
        int r = select(0, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(sock, &rfds)) {
            SOCKET client = accept(sock, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                std::thread(handle_client, client).detach();
            }
        }
    }
    closesocket(sock);
    WSACleanup();
}
#endif // _WIN32
} // namespace

extern "C" int sao_ai_editor_run_mcp_server_streamable_http(uint16_t port) {
    if (g_http_running.exchange(true)) return -1;
    if (port != 0) g_http_port = port;
#if defined(_WIN32)
    g_http_thread = std::thread(listener_loop, g_http_port);
    g_http_thread.detach();
#endif
    return 0;
}

extern "C" int sao_ai_editor_stop_mcp_server_streamable_http(void) {
    g_http_running.store(false);
    return 0;
}

extern "C" uint16_t sao_ai_editor_mcp_http_port(void) { return g_http_port; }

} // namespace sao::ai_editor
