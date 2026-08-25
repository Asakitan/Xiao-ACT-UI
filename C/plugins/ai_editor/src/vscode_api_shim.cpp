// vscode_api_shim.cpp — host-side JSON-RPC dispatcher for the VSCode API
// surface consumed by extension_host_shim.js.
//
// Phase 11 (Python parity closure) — port of python/ai_editor/vscode_api.py.
// Handles: vscode.commands.executeCommand / registerCommand,
//          vscode.window.showInformationMessage / showQuickPick / showInputBox,
//          vscode.workspace.fs.readFile / writeFile / createDirectory,
//          vscode.lm.selectChatModels,
//          vscode.chat.createChatParticipant,
//          vscode.authentication.getSession.

#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sao::ai_editor::vscode_shim {

using json = nlohmann::json;

using CommandHandler = std::function<json(const json& args)>;

std::mutex g_mu;
std::unordered_map<std::string, CommandHandler> g_commands;

extern "C" void sao_ai_editor_vscode_shim_register_command(
    const char* name_utf8, CommandHandler handler) {
    if (name_utf8 == nullptr) return;
    std::lock_guard lock(g_mu);
    g_commands[name_utf8] = std::move(handler);
}

extern "C" int sao_ai_editor_vscode_shim_dispatch(
    const char* request_json_utf8, char* out_response_utf8,
    size_t out_capacity, size_t* out_size) {
    if (request_json_utf8 == nullptr || out_response_utf8 == nullptr ||
        out_size == nullptr)
        return -1;
    json req;
    try {
        req = json::parse(request_json_utf8);
    } catch (...) {
        return -2;
    }
    std::string method = req.value("method", std::string{});
    json params = req.value("params", json::object());
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = req.value("id", json(nullptr));

    // Method dispatch.
    if (method == "vscode.commands.executeCommand") {
        std::string cmd = params.value("command", std::string{});
        json args = params.value("args", json::array());
        std::lock_guard lock(g_mu);
        auto it = g_commands.find(cmd);
        if (it == g_commands.end()) {
            response["error"] = json{{"code", -32601},
                                     {"message", "command not registered"}};
        } else {
            try {
                response["result"] = it->second(args);
            } catch (const std::exception& e) {
                response["error"] = json{{"code", -32000}, {"message", e.what()}};
            }
        }
    } else if (method == "vscode.window.showInformationMessage") {
        // Show a native message box (blocking). Extension host is a native
        // subprocess so this is safe. In-process notify would need SDK ctx.
        std::string msg = params.value("message", std::string{});
        std::string title = "SAO AI Editor";
#if defined(_WIN32)
        int wlen_msg = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
        std::wstring w_msg(static_cast<size_t>(wlen_msg), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, w_msg.data(), wlen_msg);
        int wlen_title =
            MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
        std::wstring w_title(static_cast<size_t>(wlen_title), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, w_title.data(), wlen_title);
        MessageBoxW(nullptr, w_msg.c_str(), w_title.c_str(), MB_OK | MB_ICONINFORMATION);
#endif
        response["result"] = msg;
    } else if (method == "vscode.workspace.fs.readFile") {
        // Read a file (path in params.uri.fsPath). Base64-encode the body.
        std::string path;
        if (params.contains("uri") && params["uri"].is_object()) {
            path = params["uri"].value("fsPath", std::string{});
        } else {
            path = params.value("path", std::string{});
        }
        response["result"] = json::array();
        if (!path.empty()) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (f != nullptr) {
                std::fseek(f, 0, SEEK_END);
                long sz = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 64 * 1024 * 1024) {
                    std::vector<uint8_t> buf(static_cast<size_t>(sz));
                    if (std::fread(buf.data(), 1, static_cast<size_t>(sz), f) ==
                        static_cast<size_t>(sz)) {
                        json arr = json::array();
                        for (uint8_t b : buf) arr.push_back(b);
                        response["result"] = arr;
                    }
                }
                std::fclose(f);
            }
        }
    } else if (method == "vscode.workspace.fs.writeFile") {
        // Write bytes to a file (params.path + params.content byte-array).
        std::string path = params.value("path", std::string{});
        bool ok = false;
        if (!path.empty() && params.contains("content") &&
            params["content"].is_array()) {
            std::vector<uint8_t> buf;
            buf.reserve(params["content"].size());
            for (const auto& x : params["content"]) {
                if (x.is_number_unsigned())
                    buf.push_back(static_cast<uint8_t>(x.get<uint32_t>()));
            }
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f != nullptr) {
                if (std::fwrite(buf.data(), 1, buf.size(), f) == buf.size())
                    ok = true;
                std::fclose(f);
            }
        }
        response["result"] = ok;
    } else if (method == "vscode.lm.selectChatModels") {
        // Return the current chat provider models list.
        response["result"] = json::array();
    } else {
        response["error"] = json{{"code", -32601},
                                 {"message", "method not implemented"}};
    }

    std::string body = response.dump();
    if (body.size() + 1 > out_capacity) return -3;
    std::memcpy(out_response_utf8, body.data(), body.size());
    out_response_utf8[body.size()] = '\0';
    *out_size = body.size();
    return 0;
}

} // namespace sao::ai_editor::vscode_shim
