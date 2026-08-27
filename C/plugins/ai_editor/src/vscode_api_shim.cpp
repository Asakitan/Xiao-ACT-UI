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

namespace sao::ai_editor::vscode_shim {

using json = nlohmann::json;
using CommandHandler = std::function<json(const json& args)>;

constexpr size_t kMaximumFsBytes = 16U * 1024U * 1024U;
constexpr size_t kMaximumRequestBytes = 16U * 1024U * 1024U;

std::mutex g_mu;
std::unordered_map<std::string, CommandHandler> g_commands;
std::mutex g_workspace_mu;
std::filesystem::path g_workspace_root;
bool g_workspace_initialized = false;

bool path_within(const std::filesystem::path& root,
                 const std::filesystem::path& candidate) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, root, error);
    if (error || relative.is_absolute()) {
        return false;
    }
    for (const auto& component : relative) {
        if (component == L"..") {
            return false;
        }
    }
    return true;
}

bool has_reparse_component(const std::filesystem::path& root,
                           const std::filesystem::path& candidate) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, root, error);
    if (error || !path_within(root, candidate)) {
        return true;
    }
    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const std::wstring native = current.native();
        const DWORD attributes = ::GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (::GetLastError() == ERROR_FILE_NOT_FOUND ||
                ::GetLastError() == ERROR_PATH_NOT_FOUND) {
                break;
            }
            return true;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            return true;
        }
    }
    return false;
}

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

bool final_handle_within_workspace(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;
    DWORD required = ::GetFinalPathNameByHandleW(handle, nullptr, 0,
                                                   FILE_NAME_NORMALIZED);
    if (required == 0) return false;
    std::wstring final_path(required, L'\0');
    const DWORD written = ::GetFinalPathNameByHandleW(
        handle, final_path.data(), required, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= required) return false;
    final_path.resize(written);
    if (final_path.rfind(L"\\\\?\\", 0) == 0)
        final_path.erase(0, 4);
    std::filesystem::path root;
    {
        std::lock_guard<std::mutex> guard(g_workspace_mu);
        root = g_workspace_root;
    }
    std::wstring root_path = root.native();
    while (root_path.size() > 3 &&
           (root_path.back() == L'\\' || root_path.back() == L'/'))
        root_path.pop_back();
    while (final_path.size() > 3 &&
           (final_path.back() == L'\\' || final_path.back() == L'/'))
        final_path.pop_back();
    auto lower = [](std::wstring value) {
        for (wchar_t& character : value)
            character = static_cast<wchar_t>(::towlower(character));
        return value;
    };
    const std::wstring lower_final = lower(std::move(final_path));
    const std::wstring lower_root = lower(std::move(root_path));
    return lower_final == lower_root ||
           (lower_final.size() > lower_root.size() &&
            lower_final.rfind(lower_root + L"\\", 0) == 0);
}

HANDLE open_verified_workspace_file(const std::filesystem::path& path,
                                    DWORD access, DWORD share,
                                    DWORD disposition) {
    const std::wstring native = path.native();
    HANDLE handle = ::CreateFileW(native.c_str(), access, share, nullptr,
                                   disposition, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (handle == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    if (!final_handle_within_workspace(handle)) {
        ::CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

bool resolve_workspace_path(std::string_view raw,
                            bool for_write,
                            std::filesystem::path& output) {
    if (raw.empty() || raw.size() > 32768U ||
        !sao::ai_editor::native::valid_utf8(raw) || !ensure_workspace_root()) {
        return false;
    }
    const std::wstring wide = sao::ai_editor::native::utf8_to_wide(raw);
    if (wide.empty()) {
        return false;
    }
    const std::filesystem::path requested(wide);
    if (requested.is_absolute() || requested.has_root_name() ||
        requested.has_root_directory()) {
        return false;
    }
    std::filesystem::path root;
    {
        std::lock_guard<std::mutex> guard(g_workspace_mu);
        root = g_workspace_root;
    }
    std::error_code error;
    std::filesystem::path candidate = root / requested;
    if (for_write && !std::filesystem::exists(candidate, error)) {
        error.clear();
    }
    candidate = std::filesystem::weakly_canonical(candidate, error);
    if (error || !candidate.is_absolute() || !path_within(root, candidate) ||
        has_reparse_component(root, candidate)) {
        return false;
    }
    output = candidate;
    return true;
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
        return initialize_workspace_root_locked(std::filesystem::path(wide))
            ? 0
            : -1;
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
                } else if (method == "vscode.window.showInformationMessage") {
                    const std::string message =
                        params.value("message", std::string{});
#if defined(_WIN32)
                    const std::wstring wide_message =
                        sao::ai_editor::native::utf8_to_wide(message);
                    const std::wstring wide_title = L"SAO AI Editor";
                    ::MessageBoxW(nullptr, wide_message.c_str(),
                                  wide_title.c_str(), MB_OK | MB_ICONINFORMATION);
#endif
                    response["result"] = message;
                } else if (method == "vscode.workspace.fs.readFile") {
                    const json uri = params.value("uri", json::object());
                    const std::string raw_path = uri.is_object()
                        ? uri.value("fsPath", std::string{})
                        : params.value("path", std::string{});
                    std::filesystem::path path;
                    if (!resolve_workspace_path(raw_path, false, path)) {
                        response["error"] = json{{"code", -32001},
                                                   {"message", "workspace path rejected"}};
                    } else {
                        HANDLE handle = open_verified_workspace_file(
                            path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            OPEN_EXISTING);
                        LARGE_INTEGER file_size{};
                        if (handle == INVALID_HANDLE_VALUE ||
                            !::GetFileSizeEx(handle, &file_size) ||
                            file_size.QuadPart < 0 ||
                            static_cast<uint64_t>(file_size.QuadPart) > kMaximumFsBytes) {
                            if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
                            response["error"] = json{{"code", -32000},
                                                       {"message", "file exceeds the bounded read limit"}};
                        } else {
                            std::vector<uint8_t> bytes(
                                static_cast<size_t>(file_size.QuadPart));
                            DWORD read = 0;
                            const bool read_ok = bytes.empty() ||
                                (::ReadFile(handle, bytes.data(),
                                             static_cast<DWORD>(bytes.size()),
                                             &read, nullptr) != FALSE &&
                                 read == bytes.size());
                            ::CloseHandle(handle);
                            if (!read_ok) {
                                response["error"] = json{{"code", -32000},
                                                           {"message", "file read failed"}};
                            } else {
                                response["result"] = json::array();
                                for (const uint8_t byte : bytes)
                                    response["result"].push_back(byte);
                            }
                        }
                    }
                } else if (method == "vscode.workspace.fs.writeFile") {
                    const std::string raw_path = params.value("path", std::string{});
                    std::filesystem::path path;
                    if (!resolve_workspace_path(raw_path, true, path) ||
                        !params.contains("content") ||
                        !params["content"].is_array() ||
                        params["content"].size() > kMaximumFsBytes) {
                        response["error"] = json{{"code", -32001},
                                                   {"message", "workspace write rejected"}};
                    } else {
                        std::vector<uint8_t> bytes;
                        bytes.reserve(params["content"].size());
                        bool valid = true;
                        for (const auto& value : params["content"]) {
                            if (!value.is_number_integer()) {
                                valid = false;
                                break;
                            }
                            if (value.is_number_unsigned()) {
                                const uint64_t byte = value.get<uint64_t>();
                                if (byte > 255U) { valid = false; break; }
                                bytes.push_back(static_cast<uint8_t>(byte));
                            } else {
                                const int64_t byte = value.get<int64_t>();
                                if (byte < 0 || byte > 255) { valid = false; break; }
                                bytes.push_back(static_cast<uint8_t>(byte));
                            }
                        }
                        if (!valid) {
                            response["error"] = json{{"code", -32602},
                                                       {"message", "content must contain byte values 0..255"}};
                        } else {
                            std::error_code error;
                            std::filesystem::create_directories(path.parent_path(), error);
                            HANDLE handle = error ? INVALID_HANDLE_VALUE
                                : open_verified_workspace_file(
                                    path, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    OPEN_ALWAYS);
                            LARGE_INTEGER zero{};
                            const bool positioned =
                                handle != INVALID_HANDLE_VALUE &&
                                ::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) &&
                                ::SetEndOfFile(handle);
                            DWORD written = 0;
                            const bool write_ok = bytes.empty() ||
                                (positioned && ::WriteFile(handle, bytes.data(),
                                    static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                                 written == bytes.size());
                            if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
                            if (!write_ok) {
                                response["error"] = json{{"code", -32000},
                                                           {"message", "file write failed"}};
                            } else {
                                response["result"] = true;
                            }
                        }
                    }
                } else if (method == "vscode.lm.selectChatModels") {
                    response["result"] = json::array();
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
