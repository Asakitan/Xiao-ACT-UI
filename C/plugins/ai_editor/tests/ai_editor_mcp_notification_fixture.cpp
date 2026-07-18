// Minimal stdio MCP server used by the notification-forwarding tests.
//
// Speaks the LSP-style `Content-Length: N\r\n\r\n<json>` frame protocol on
// stdin/stdout.  Handles just enough of the JSON-RPC surface to complete the
// client's initialize handshake, and then pushes one or more notifications
// (default: notifications/tools/list_changed) to prove the client wires them
// through to the caller.
//
// Command-line switches:
//   --notify-count N      how many notifications to push after initialize
//                         (default 1)
//   --notify-method M     JSON-RPC method to use (default
//                         "notifications/tools/list_changed")
//   --pre-notify-delay-ms K   pause K ms after initialize before pushing the
//                         first notification (default 0)
//   --hold-open-ms K      keep the fixture alive K ms after pushing so the
//                         client has time to observe (default 500)
//
// The fixture keeps its footprint tiny — no nlohmann/json dependency — because
// the interesting behaviour lives in the client under test.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string read_frame(HANDLE stdin_handle) {
    std::string headers;
    // Slurp the header block one byte at a time — the framed protocol never
    // hands us more than a couple hundred bytes per header, and reading in
    // chunks would require a re-scan for the \r\n\r\n boundary.
    while (true) {
        char byte = 0;
        DWORD read = 0;
        if (!ReadFile(stdin_handle, &byte, 1, &read, nullptr) || read == 0) {
            return {};
        }
        headers.push_back(byte);
        const size_t size = headers.size();
        if (size >= 4 &&
            headers[size - 4] == '\r' && headers[size - 3] == '\n' &&
            headers[size - 2] == '\r' && headers[size - 1] == '\n') {
            break;
        }
    }
    // Extract the Content-Length header (case-insensitive lookup).
    size_t length = 0;
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(::tolower(c));
                   });
    const size_t marker = lower.find("content-length:");
    if (marker == std::string::npos) {
        return {};
    }
    size_t cursor = marker + std::strlen("content-length:");
    while (cursor < headers.size() &&
           (headers[cursor] == ' ' || headers[cursor] == '\t')) {
        ++cursor;
    }
    while (cursor < headers.size() &&
           headers[cursor] >= '0' && headers[cursor] <= '9') {
        length = length * 10U + static_cast<size_t>(headers[cursor] - '0');
        ++cursor;
    }
    if (length == 0 || length > 4U * 1024U * 1024U) {
        return {};
    }
    std::string body(length, '\0');
    size_t total = 0;
    while (total < length) {
        DWORD read = 0;
        if (!ReadFile(stdin_handle, body.data() + total,
                      static_cast<DWORD>(length - total), &read, nullptr) ||
            read == 0) {
            return {};
        }
        total += read;
    }
    return body;
}

bool write_frame(HANDLE stdout_handle, std::string_view payload) {
    std::string header = "Content-Length: " + std::to_string(payload.size()) +
                          "\r\n\r\n";
    DWORD written = 0;
    if (!WriteFile(stdout_handle, header.data(),
                   static_cast<DWORD>(header.size()), &written, nullptr) ||
        written != header.size()) {
        return false;
    }
    written = 0;
    return WriteFile(stdout_handle, payload.data(),
                     static_cast<DWORD>(payload.size()), &written, nullptr) &&
           written == payload.size();
}

// Yank out the integer JSON-RPC id verbatim so we can echo it in responses
// without parsing the whole envelope.  Returns "1" when the id is missing (the
// client's initialize call always sends one, so this is defensive only).
std::string extract_id(std::string_view body) {
    const size_t marker = body.find("\"id\"");
    if (marker == std::string_view::npos) {
        return "1";
    }
    size_t cursor = marker + 4;
    while (cursor < body.size() &&
           (body[cursor] == ' ' || body[cursor] == ':' || body[cursor] == '\t')) {
        ++cursor;
    }
    std::string id;
    while (cursor < body.size() &&
           ((body[cursor] >= '0' && body[cursor] <= '9') ||
            body[cursor] == '-')) {
        id.push_back(body[cursor]);
        ++cursor;
    }
    return id.empty() ? std::string{"1"} : id;
}

bool contains_method(std::string_view body, std::string_view method) {
    return body.find(std::string("\"method\":\"") + std::string(method) +
                     "\"") != std::string_view::npos;
}

int parse_int_arg(int argc, wchar_t** argv, const wchar_t* flag,
                  int fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wcscmp(argv[index], flag) == 0) {
            return std::stoi(argv[index + 1]);
        }
    }
    return fallback;
}

std::string parse_utf8_arg(int argc, wchar_t** argv, const wchar_t* flag,
                           std::string fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wcscmp(argv[index], flag) == 0) {
            const int required = WideCharToMultiByte(
                CP_UTF8, 0, argv[index + 1], -1, nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return fallback;
            }
            std::string out(static_cast<size_t>(required) - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, argv[index + 1], -1, out.data(),
                                required, nullptr, nullptr);
            return out;
        }
    }
    return fallback;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int notify_count = std::max(0, parse_int_arg(argc, argv,
                                                        L"--notify-count", 1));
    const int pre_delay_ms = std::max(0, parse_int_arg(argc, argv,
                                                        L"--pre-notify-delay-ms",
                                                        0));
    const int hold_open_ms = std::max(0, parse_int_arg(argc, argv,
                                                        L"--hold-open-ms", 500));
    const std::string notify_method = parse_utf8_arg(
        argc, argv, L"--notify-method",
        std::string{"notifications/tools/list_changed"});

    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE ||
        stdout_handle == INVALID_HANDLE_VALUE) {
        return 2;
    }

    bool initialized = false;
    while (true) {
        const std::string frame = read_frame(stdin_handle);
        if (frame.empty()) {
            break;
        }
        if (contains_method(frame, "initialize")) {
            const std::string id = extract_id(frame);
            const std::string reply =
                std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
                ",\"result\":{\"protocolVersion\":\"2024-11-05\","
                "\"serverInfo\":{\"name\":\"fake-mcp\",\"version\":\"1.0\"},"
                "\"capabilities\":{\"tools\":{\"listChanged\":true}}}}";
            if (!write_frame(stdout_handle, reply)) {
                return 3;
            }
            continue;
        }
        if (contains_method(frame, "notifications/initialized")) {
            // Client-to-server initialized notification; no reply expected.
            // Once it lands we know the handshake is complete and we can
            // start pushing our own notifications back.
            initialized = true;
            if (pre_delay_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(pre_delay_ms));
            }
            for (int index = 0; index < notify_count; ++index) {
                const std::string notification =
                    std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") +
                    notify_method +
                    "\",\"params\":{\"sequence\":" + std::to_string(index) +
                    "}}";
                if (!write_frame(stdout_handle, notification)) {
                    return 4;
                }
            }
            // Give the client's reader thread a moment to observe.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(hold_open_ms));
            continue;
        }
        if (contains_method(frame, "shutdown")) {
            break;
        }
        // Unknown request/method — reply with a benign error so the client
        // does not deadlock on a missing response.
        const std::string id = extract_id(frame);
        const std::string reply =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
            ",\"error\":{\"code\":-32601,\"message\":\"unsupported\"}}";
        (void)write_frame(stdout_handle, reply);
    }

    return initialized ? 0 : 5;
}
