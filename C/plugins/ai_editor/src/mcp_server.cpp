#include "mcp_server.h"

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/mcp_codec.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::native {
namespace {

using Json = nlohmann::json;

constexpr uint32_t kReadChunkBytes = 64U * 1024U;
constexpr uint32_t kMaxMessageBytes = 4U * 1024U * 1024U;

std::string wide_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

int32_t forward_dispatch(sao_ai_editor_runtime_t runtime, const Json& request,
                         Json& response) {
    const std::string encoded = request.dump();
    uint32_t required = 0;
    const int32_t query = sao_ai_editor_runtime_dispatch(
        runtime, encoded.data(), static_cast<uint32_t>(encoded.size()), nullptr,
        0, &required);
    if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return query;
    }
    if (required == 0 || required > kMaxMessageBytes) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    std::vector<char> buffer(static_cast<size_t>(required) + 1U, '\0');
    uint32_t written = 0;
    const int32_t drain = sao_ai_editor_runtime_dispatch(
        runtime, nullptr, 0, buffer.data(),
        static_cast<uint32_t>(buffer.size()), &written);
    if (drain != SAO_AI_EDITOR_OK) {
        return drain;
    }
    response = Json::parse(buffer.data(), buffer.data() + written, nullptr,
                           false);
    return response.is_discarded() ? SAO_AI_EDITOR_ERR_PROTOCOL
                                   : SAO_AI_EDITOR_OK;
}

int32_t tools_schema(sao_ai_editor_runtime_t runtime, Json& out) {
    Json request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools.list"},
                 {"params", {{"mode", "agent"}}}};
    Json inner;
    const int32_t status = forward_dispatch(runtime, request, inner);
    if (status != SAO_AI_EDITOR_OK || inner.contains("error")) return SAO_AI_EDITOR_ERR_PROTOCOL;
    const Json runtime_result = inner.value("result", Json::object());
    const Json descriptors = runtime_result.value("tools", Json());
    if (!descriptors.is_array()) return SAO_AI_EDITOR_ERR_PROTOCOL;
    out = Json::array();
    for (const auto& descriptor : descriptors) {
        if (!descriptor.is_object() || !descriptor.contains("name") || !descriptor["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        Json input_schema = descriptor.value("parameters", Json::object());
        if (!input_schema.is_object()) {
            input_schema = Json{{"type", "object"}, {"properties", Json::object()}};
        }
        const bool read_only = descriptor.value("readOnly", false);
        out.push_back(Json{{"name", descriptor["name"]},
                           {"description", descriptor.value("description", std::string{})},
                           {"inputSchema", std::move(input_schema)},
                           {"annotations", {{"readOnlyHint", read_only}}}});
    }
    return SAO_AI_EDITOR_OK;
}

Json error_envelope(const Json& id, int code, std::string_view message) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", code}, {"message", std::string(message)}}}};
}

Json result_envelope(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

int32_t handle_message(sao_ai_editor_runtime_t runtime, const Json& request,
                       Json& response) {
    if (!request.is_object() || !request.contains("method") ||
        !request["method"].is_string()) {
        response = error_envelope(request.value("id", Json(nullptr)), -32600,
                                  "invalid MCP message");
        return SAO_AI_EDITOR_OK;
    }
    const std::string method = request["method"].get<std::string>();
    const Json id = request.value("id", Json(nullptr));
    if (method == "initialize") {
        response = result_envelope(
            id,
            Json{{"protocolVersion", "2024-11-05"},
                 {"capabilities",
                  Json{{"tools", Json::object()}, {"resources",
                                                   Json::object()}}},
                 {"serverInfo",
                  Json{{"name", "sao-ai-editor"}, {"version", "1.0"}}}});
        return SAO_AI_EDITOR_OK;
    }
    if (method == "shutdown") {
        response = result_envelope(id, Json::object());
        return SAO_AI_EDITOR_OK;
    }
    if (method == "notifications/initialized" ||
        method == "notifications/cancelled") {
        // Notifications carry no id; silently accept.
        response = Json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools/list") {
        Json tools;
        if (tools_schema(runtime, tools) != SAO_AI_EDITOR_OK) {
            response = error_envelope(id, -32000, "runtime tools discovery failed");
            return SAO_AI_EDITOR_OK;
        }
        response = result_envelope(id, Json{{"tools", std::move(tools)}});
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools/call") {
        const Json params = request.value("params", Json::object());
        if (!params.is_object() || !params.contains("name") ||
            !params["name"].is_string()) {
            response = error_envelope(id, -32602, "params.name required");
            return SAO_AI_EDITOR_OK;
        }
        const Json arguments = params.value("arguments", Json::object());
        Json forwarded{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", "tools.call"},
                       {"params", {{"mode", "agent"},
                                   {"name", params["name"]},
                                   {"arguments", arguments}}}};
        Json inner;
        const int32_t status = forward_dispatch(runtime, forwarded, inner);
        if (status != SAO_AI_EDITOR_OK) {
            response = error_envelope(id, -32000, "dispatch failed");
            return status;
        }
        if (inner.contains("error")) {
            const std::string message =
                inner["error"].value("message", "tool failed");
            response = error_envelope(id, -32000, message);
            return SAO_AI_EDITOR_OK;
        }
        response = result_envelope(
            id,
            Json{{"content",
                  Json::array({Json{{"type", "text"},
                                    {"text", inner.value("result",
                                                          Json::object()).dump(2)}}})}});
        return SAO_AI_EDITOR_OK;
    }
    if (method == "resources/list") {
        Json forwarded{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", "tools.call"},
                       {"params", {{"mode", "agent"},
                                   {"name", "listFiles"},
                                   {"arguments",
                                    {{"path", "."},
                                     {"recursive", true},
                                     {"limit", 500}}}}}};
        Json inner;
        const int32_t status = forward_dispatch(runtime, forwarded, inner);
        if (status != SAO_AI_EDITOR_OK) {
            response = error_envelope(id, -32000, "list resources failed");
            return status;
        }
        Json entries = Json::array();
        if (inner.contains("result") &&
            inner["result"].contains("entries") &&
            inner["result"]["entries"].is_array()) {
            for (const auto& entry : inner["result"]["entries"]) {
                if (!entry.is_object() ||
                    entry.value("type", "") != "file") {
                    continue;
                }
                const std::string name = entry.value("name", "");
                entries.push_back(
                    Json{{"uri", "sao://workspace/" + name},
                         {"name", name},
                         {"mimeType", "text/plain"}});
            }
        }
        response = result_envelope(id, Json{{"resources", std::move(entries)}});
        return SAO_AI_EDITOR_OK;
    }
    if (method == "resources/read") {
        const Json params = request.value("params", Json::object());
        const std::string uri = params.value("uri", "");
        constexpr std::string_view prefix = "sao://workspace/";
        if (uri.rfind(prefix, 0) != 0) {
            response = error_envelope(id, -32602, "unsupported uri");
            return SAO_AI_EDITOR_OK;
        }
        const std::string path = uri.substr(prefix.size());
        Json forwarded{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", "tools.call"},
                       {"params", {{"mode", "agent"},
                                   {"name", "readFile"},
                                   {"arguments", {{"path", path}}}}}};
        Json inner;
        const int32_t status = forward_dispatch(runtime, forwarded, inner);
        if (status != SAO_AI_EDITOR_OK || inner.contains("error")) {
            response = error_envelope(id, -32000,
                                       inner.contains("error")
                                           ? inner["error"].value(
                                                 "message", "read failed")
                                           : "read failed");
            return status;
        }
        response = result_envelope(
            id,
            Json{{"contents",
                  Json::array({Json{{"uri", uri},
                                    {"mimeType", "text/plain"},
                                    {"text", inner["result"].value("content",
                                                                    "")}}})}});
        return SAO_AI_EDITOR_OK;
    }
    response = error_envelope(id, -32601, "method not found");
    return SAO_AI_EDITOR_OK;
}

bool write_all(HANDLE handle, const void* data, size_t size) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(size, static_cast<size_t>(64U * 1024U)));
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

bool send_response(HANDLE stdout_handle, const Json& response) {
    if (response.is_null()) {
        return true;
    }
    const std::string payload = response.dump();
    if (payload.size() > kMaxMessageBytes) {
        return false;
    }
    const std::string header =
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    return write_all(stdout_handle, header.data(), header.size()) &&
           write_all(stdout_handle, payload.data(), payload.size());
}

}  // namespace

int run_mcp_server_stdio(const std::filesystem::path& workspace_root) {
    SaoAiEditorRuntimeConfig config{};
    config.struct_size = sizeof(config);
    const std::string workspace_utf8 = wide_utf8(workspace_root.native());
    if (workspace_utf8.empty()) {
        return 4;
    }
    config.workspace_root_utf8 = workspace_utf8.c_str();
    sao_ai_editor_runtime_t runtime = nullptr;
    if (sao_ai_editor_runtime_create(&config, &runtime) != SAO_AI_EDITOR_OK ||
        runtime == nullptr) {
        return 7;
    }
    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE ||
        stdout_handle == INVALID_HANDLE_VALUE) {
        sao_ai_editor_runtime_destroy(runtime);
        return 5;
    }

    sao_ai_editor_mcp_decoder_t decoder = nullptr;
    if (sao_ai_editor_mcp_decoder_create(kMaxMessageBytes, &decoder) !=
        SAO_AI_EDITOR_OK) {
        sao_ai_editor_runtime_destroy(runtime);
        return 6;
    }

    std::vector<char> buffer(kReadChunkBytes);
    int exit_code = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(stdin_handle, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
            break;
        }
        uint32_t required = 0;
        int32_t status = sao_ai_editor_mcp_decoder_feed(
            decoder, buffer.data(), read, nullptr, 0, &required);
        if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL || required == 0) {
            continue;
        }
        std::vector<char> messages(static_cast<size_t>(required) + 1U);
        status = sao_ai_editor_mcp_decoder_feed(
            decoder, nullptr, 0, messages.data(),
            static_cast<uint32_t>(messages.size()), &required);
        if (status != SAO_AI_EDITOR_OK) {
            continue;
        }
        Json parsed = Json::parse(messages.data(), messages.data() + required,
                                  nullptr, false);
        if (!parsed.is_array()) {
            continue;
        }
        bool should_exit = false;
        for (auto& message : parsed) {
            if (!message.is_object()) {
                continue;
            }
            Json response;
            (void)handle_message(runtime, message, response);
            if (message.value("method", "") == "shutdown") {
                should_exit = true;
            }
            if (!send_response(stdout_handle, response)) {
                should_exit = true;
                exit_code = 8;
                break;
            }
        }
        if (should_exit) {
            break;
        }
    }
    sao_ai_editor_mcp_decoder_destroy(decoder);
    sao_ai_editor_runtime_destroy(runtime);
    return exit_code;
}

namespace {

// Send a raw JSON-RPC response using Content-Length framing.  Empty
// strings are treated as notifications (no frame emitted) so callers can
// short-circuit responses for JSON-RPC notification requests.
bool send_extension_host_frame(HANDLE stdout_handle,
                               std::string_view payload) {
    if (payload.empty()) {
        return true;
    }
    if (payload.size() > kMaxMessageBytes) {
        return false;
    }
    const std::string header =
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    return write_all(stdout_handle, header.data(), header.size()) &&
           write_all(stdout_handle, payload.data(), payload.size());
}

// Best-effort implicit `extensions.configure_host` before entering the
// request loop.  Failure is non-fatal: the parent can always re-issue an
// explicit configure_host with its own params.  Returns SAO_AI_EDITOR_OK
// if the runtime accepted the configuration or the hint was empty.
int32_t apply_node_executable_hint(sao_ai_editor_runtime_t runtime,
                                    const std::filesystem::path&
                                        node_executable_hint) {
    if (node_executable_hint.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    const std::string node_utf8 = wide_utf8(node_executable_hint.native());
    if (node_utf8.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json request{
        {"jsonrpc", "2.0"},
        {"id", 0},
        {"method", "extensions.configure_host"},
        {"params", Json{{"nodeExecutable", node_utf8}}}};
    Json response;
    return forward_dispatch(runtime, request, response);
}

// Parse a JSON-RPC method string cheaply — used to detect the
// `host.shutdown` sentinel that terminates the request loop after a
// successful reply is written.
std::string extract_method_name(const Json& message) {
    if (!message.is_object() || !message.contains("method") ||
        !message["method"].is_string()) {
        return {};
    }
    return message["method"].get<std::string>();
}

}  // namespace

int run_extension_host_stdio(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& node_executable_hint) {
    SaoAiEditorRuntimeConfig config{};
    config.struct_size = sizeof(config);
    const std::string workspace_utf8 = wide_utf8(workspace_root.native());
    if (workspace_utf8.empty()) {
        return 4;
    }
    config.workspace_root_utf8 = workspace_utf8.c_str();
    sao_ai_editor_runtime_t runtime = nullptr;
    if (sao_ai_editor_runtime_create(&config, &runtime) != SAO_AI_EDITOR_OK ||
        runtime == nullptr) {
        return 7;
    }

    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE ||
        stdout_handle == INVALID_HANDLE_VALUE) {
        sao_ai_editor_runtime_destroy(runtime);
        return 5;
    }

    // Apply the node executable hint before the request loop opens so the
    // parent can immediately register/activate extensions without an
    // explicit configure_host round-trip.  A malformed hint is a
    // command-line error; a well-formed hint that the runtime rejects
    // (e.g. shim missing) is not — the parent can retry configure_host.
    const int32_t hint_status =
        apply_node_executable_hint(runtime, node_executable_hint);
    if (hint_status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
        sao_ai_editor_runtime_destroy(runtime);
        return 4;
    }

    sao_ai_editor_mcp_decoder_t decoder = nullptr;
    if (sao_ai_editor_mcp_decoder_create(kMaxMessageBytes, &decoder) !=
        SAO_AI_EDITOR_OK) {
        sao_ai_editor_runtime_destroy(runtime);
        return 6;
    }

    std::vector<char> buffer(kReadChunkBytes);
    int exit_code = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(stdin_handle, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
            break;
        }
        uint32_t required = 0;
        int32_t status = sao_ai_editor_mcp_decoder_feed(
            decoder, buffer.data(), read, nullptr, 0, &required);
        if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL || required == 0) {
            continue;
        }
        std::vector<char> messages(static_cast<size_t>(required) + 1U);
        status = sao_ai_editor_mcp_decoder_feed(
            decoder, nullptr, 0, messages.data(),
            static_cast<uint32_t>(messages.size()), &required);
        if (status != SAO_AI_EDITOR_OK) {
            continue;
        }
        Json parsed = Json::parse(messages.data(), messages.data() + required,
                                  nullptr, false);
        if (!parsed.is_array()) {
            continue;
        }
        bool should_exit = false;
        for (const auto& message : parsed) {
            if (!message.is_object()) {
                continue;
            }
            const std::string method = extract_method_name(message);
            // host.shutdown is our extension host stdio sentinel — reply
            // OK then break out of the loop so ~NativeRuntime triggers
            // extension_host teardown (node_runtime -> deactivate all).
            if (method == "host.shutdown") {
                const Json ok = Json{
                    {"jsonrpc", "2.0"},
                    {"id", message.value("id", Json(nullptr))},
                    {"result", Json::object()}};
                (void)send_extension_host_frame(stdout_handle, ok.dump());
                should_exit = true;
                break;
            }
            // Notifications (no id) get dispatched but never reply — the
            // NativeRuntime already emits its own error envelope for
            // unknown methods, but the standard says notifications must
            // not have responses.
            const bool is_notification =
                !message.contains("id") || message["id"].is_null();

            Json response;
            const int32_t dispatch_status =
                forward_dispatch(runtime, message, response);
            if (is_notification) {
                continue;
            }
            std::string payload;
            if (dispatch_status != SAO_AI_EDITOR_OK) {
                payload = Json{
                    {"jsonrpc", "2.0"},
                    {"id", message.value("id", Json(nullptr))},
                    {"error",
                     {{"code", -32000},
                      {"message", "runtime dispatch failed"},
                      {"data", {{"status", dispatch_status}}}}}}.dump();
            } else {
                payload = response.dump();
            }
            if (!send_extension_host_frame(stdout_handle, payload)) {
                should_exit = true;
                exit_code = 8;
                break;
            }
        }
        if (should_exit) {
            break;
        }
    }
    sao_ai_editor_mcp_decoder_destroy(decoder);
    // Explicit destroy so the ExtensionHost destructor (which shuts down
    // the Node.js child and deactivates loaded extensions) runs before
    // the process exits.
    sao_ai_editor_runtime_destroy(runtime);
    return exit_code;
}

}  // namespace sao::ai_editor::native
