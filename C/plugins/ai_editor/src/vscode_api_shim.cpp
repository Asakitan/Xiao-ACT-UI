// vscode_api_shim.cpp — host-side JSON-RPC dispatcher for the VSCode API
// surface consumed by extension_host_shim.js.

#include <cstdio>
#include <cstdint>
#include <exception>
#include <cstring>

#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "native_utils.h"
#include "native_runtime_internal.h"

namespace sao::ai_editor::vscode_shim {

using json = nlohmann::json;
using CommandHandler = std::function<json(const json& args)>;

constexpr size_t kMaximumRequestBytes = 16U * 1024U * 1024U;

std::mutex g_mu;
std::unordered_map<std::string, CommandHandler> g_commands;
std::mutex g_workspace_mu;
std::filesystem::path g_workspace_root;
bool g_workspace_initialized = false;

bool initialize_workspace_root_locked(const std::filesystem::path& requested) {
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(requested, error);
    if (error || !absolute.is_absolute()) {
        return false;
    }
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(absolute, error);
    if (error || !canonical.is_absolute() ||
        !std::filesystem::is_directory(canonical, error) || error) {
        return false;
    }
    g_workspace_root = canonical;
    g_workspace_initialized = true;
    return true;
}

bool ensure_workspace_root() {
    std::lock_guard<std::mutex> guard(g_workspace_mu);
    if (g_workspace_initialized) {
        return true;
    }
    return initialize_workspace_root_locked(std::filesystem::current_path());
}

json protocol_error(int code, std::string_view message) {
    return json{{"jsonrpc", "2.0"}, {"id", nullptr},
                {"error", {{"code", code},
                            {"message", std::string(message)}}}};
}

extern "C" void sao_ai_editor_vscode_shim_register_command(
    const char* name_utf8, CommandHandler handler) {
    if (name_utf8 == nullptr) return;
    std::lock_guard<std::mutex> guard(g_mu);
    g_commands[name_utf8] = std::move(handler);
}

extern "C" int sao_ai_editor_vscode_shim_set_workspace_root(
    const char* root_utf8) {
    try {
        if (root_utf8 == nullptr ||
            !sao::ai_editor::native::valid_utf8(root_utf8)) {
            return -1;
        }
        const std::wstring wide =
            sao::ai_editor::native::utf8_to_wide(root_utf8);
        if (wide.empty()) {
            return -1;
        }
        std::lock_guard<std::mutex> guard(g_workspace_mu);
        const bool ok = initialize_workspace_root_locked(std::filesystem::path(wide));
        if (ok) {
            // Bind the shared extapi surface to the same workspace so the
            // standalone dispatch door resolves identical state.  The system
            // root defaults to %USERPROFILE%\.sao until a real runtime
            // attaches and re-configures it.
            sao::ai_editor::native::extapi::configure(root_utf8, "", "");
        }
        return ok ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

extern "C" int sao_ai_editor_vscode_shim_dispatch(
    const char* request_json_utf8, char* out_response_utf8,
    size_t out_capacity, size_t* out_size) {
    if (request_json_utf8 == nullptr || out_size == nullptr) {
        return -1;
    }
    *out_size = 0;
    json response;
    bool notification = false;
    try {
        const std::string_view request_view(request_json_utf8);
        if (request_view.size() > kMaximumRequestBytes ||
            !sao::ai_editor::native::valid_utf8(request_view)) {
            response = protocol_error(-32600, "invalid request");
        } else {
            const json req = json::parse(request_view, nullptr, false);
            if (!req.is_object() || !req.contains("jsonrpc") ||
                !req["jsonrpc"].is_string() || req["jsonrpc"] != "2.0") {
                response = protocol_error(-32600, "invalid request");
            } else {
                notification = !req.contains("id");
                const std::string method = req.value("method", std::string{});
                const json params = req.value("params", json::object());
                response = json{{"jsonrpc", "2.0"},
                                {"id", req.value("id", json(nullptr))}};
                if (method == "vscode.commands.executeCommand") {
                    const std::string command =
                        params.value("command", std::string{});
                    const json args = params.value("args", json::array());
                    CommandHandler handler;
                    {
                        std::lock_guard<std::mutex> guard(g_mu);
                        const auto found = g_commands.find(command);
                        if (found != g_commands.end()) {
                            handler = found->second;
                        }
                    }
                    if (!handler) {
                        response["error"] = json{{"code", -32601},
                                                   {"message", "command not registered"}};
                    } else {
                        try {
                            response["result"] = handler(args);
                        } catch (const std::exception& error) {
                            response["error"] = json{{"code", -32000},
                                                       {"message", error.what()}};
                        }
                    }
                } else if (method == "vscode.window.showInformationMessage" ||
                           method == "vscode.window.showWarningMessage" ||
                           method == "vscode.window.showErrorMessage") {
                    CommandHandler handler;
                    {
                        std::lock_guard<std::mutex> guard(g_mu);
                        const auto found = g_commands.find(method);
                        if (found != g_commands.end()) handler = found->second;
                    }
                    if (handler) response["result"] = handler(params);
                    else response["error"] = json{{"code", -32002},
                        {"message", "No compositor notification host is registered."}};
                } else if (method == "vscode.workspace.fs.readFile" ||
                           method == "vscode.workspace.fs.writeFile") {
                    std::filesystem::path root;
                    if (ensure_workspace_root()) {
                        std::lock_guard<std::mutex> guard(g_workspace_mu);
                        root = g_workspace_root;
                    }
                    json out;
                    const int32_t status = sao::ai_editor::native::workspace_binary_file_io(
                        root, params, method == "vscode.workspace.fs.writeFile", out);
                    if (status == SAO_AI_EDITOR_OK) {
                        response["result"] = std::move(out);
                    } else {
                        const int code = status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT ? -32602
                            : status == SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION ? -32001
                            : status == SAO_AI_EDITOR_ERR_NOT_INITIALIZED ? -32002 : -32000;
                        response["error"] = json{{"code", code},
                            {"message", out.value("message", std::string{"workspace file I/O failed"})},
                            {"data", json{{"status", status}}}};
                    }
                } else if (method == "vscode.lm.selectChatModels") {
                    // Real catalog comes from extapi (empty on the
                    // standalone door until providers are configured — the
                    // registry persists under the configured roots).
                    sao::ai_editor::native::Json out;
                    const int32_t status =
                        sao::ai_editor::native::extapi::dispatch(nullptr, method,
                                                               params, out);
                    if (status == SAO_AI_EDITOR_OK) {
                        response["result"] = out;
                    } else {
                        response["error"] = json{
                            {"code", -32000},
                            {"message", "selectChatModels failed"},
                            {"data", json{{"status", status}}}};
                    }
                } else if (method.rfind("vscode.", 0) == 0 ||
                           method.rfind("sao.extapi.", 0) == 0) {
                    // Shared extension-API surface — identical dispatch as
                    // NativeRuntime::dispatch_extension_call's fallback.
                    sao::ai_editor::native::Json out;
                    const int32_t status =
                        sao::ai_editor::native::extapi::dispatch(nullptr, method,
                                                               params, out);
                    if (status == SAO_AI_EDITOR_OK) {
                        response["result"] = out;
                    } else {
                        int code = -32000;
                        if (status ==
                            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
                            code = -32602;
                        } else if (status ==
                                   SAO_AI_EDITOR_ERR_NOT_FOUND) {
                            code = -32601;
                        } else if (status ==
                                   SAO_AI_EDITOR_ERR_NOT_INITIALIZED) {
                            code = -32002;
                        }
                        const std::string message =
                            out.is_object()
                                ? out.value("message",
                                            std::string{"method not implemented"})
                                : std::string{"method not implemented"};
                        response["error"] = json{
                            {"code", code},
                            {"message", message},
                            {"data", json{{"status", status}}}};
                    }
                } else {
                    response["error"] = json{{"code", -32601},
                                               {"message", "method not implemented"}};
                }
            }
        }
    } catch (const std::exception& error) {
        response = protocol_error(-32000, error.what());
    } catch (...) {
        response = protocol_error(-32000, "vscode shim dispatch failed");
    }
    if (notification) {
        return 0;
    }
    if (out_response_utf8 == nullptr) {
        return -1;
    }
    const std::string body = response.dump();
    if (body.size() + 1U > out_capacity) {
        return -3;
    }
    std::memcpy(out_response_utf8, body.data(), body.size());
    out_response_utf8[body.size()] = '\0';
    *out_size = body.size();
    return 0;
}

} // namespace sao::ai_editor::vscode_shim
