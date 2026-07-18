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

Json tools_schema() {
    return Json::array({
        Json{{"name", "readFile"},
             {"description", "Read a UTF-8 file in the SAO workspace"},
             {"inputSchema",
              {{"type", "object"},
               {"properties",
                {{"path", {{"type", "string"}}},
                 {"startLine", {{"type", "integer"}}},
                 {"endLine", {{"type", "integer"}}}}},
               {"required", {"path"}}}}},
        Json{{"name", "listFiles"},
             {"description", "List SAO workspace files and directories"},
             {"inputSchema",
              {{"type", "object"},
               {"properties",
                {{"path", {{"type", "string"}}},
                 {"pattern", {{"type", "string"}}},
                 {"recursive", {{"type", "boolean"}}},
                 {"limit", {{"type", "integer"}}}}}}}},
        Json{{"name", "searchFiles"},
             {"description", "Search UTF-8 SAO workspace files"},
             {"inputSchema",
              {{"type", "object"},
               {"properties",
                {{"query", {{"type", "string"}}},
                 {"path", {{"type", "string"}}},
                 {"pattern", {{"type", "string"}}},
                 {"regex", {{"type", "boolean"}}},
                 {"caseSensitive", {{"type", "boolean"}}},
                 {"limit", {{"type", "integer"}}}}},
               {"required", {"query"}}}}},
        Json{{"name", "editFile"},
             {"description", "Create or replace a UTF-8 SAO workspace file"},
             {"inputSchema",
              {{"type", "object"},
               {"properties",
                {{"path", {{"type", "string"}}},
                 {"content", {{"type", "string"}}},
                 {"startLine", {{"type", "integer"}}},
                 {"endLine", {{"type", "integer"}}}}},
               {"required", {"path", "content"}}}}},
    });
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
        response = result_envelope(id, Json{{"tools", tools_schema()}});
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

}  // namespace sao::ai_editor::native
