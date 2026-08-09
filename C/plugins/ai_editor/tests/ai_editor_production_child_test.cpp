#include <catch2/catch_test_macros.hpp>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_status.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef SAO_AI_EDITOR_PRODUCTION_EXECUTABLE
#define SAO_AI_EDITOR_PRODUCTION_EXECUTABLE ""
#endif

#ifndef SAO_AI_EDITOR_HAS_WEBVIEW
#define SAO_AI_EDITOR_HAS_WEBVIEW 0
#endif

#ifndef SAO_AI_EDITOR_MAIN_SOURCE
#define SAO_AI_EDITOR_MAIN_SOURCE ""
#endif

#ifndef SAO_AI_EDITOR_CMAKE_SOURCE
#define SAO_AI_EDITOR_CMAKE_SOURCE ""
#endif

#ifndef SAO_AI_EDITOR_WEBVIEW_SOURCE
#define SAO_AI_EDITOR_WEBVIEW_SOURCE ""
#endif

namespace {

using Json = nlohmann::json;

struct TemporaryDirectory final {
    TemporaryDirectory() {
        wchar_t root[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, root) > 0);
        std::wstring directory_name(L"sao-ai-editor-production-child-");
        directory_name += std::to_wstring(GetCurrentProcessId());
        directory_name += L"-";
        directory_name += std::to_wstring(GetTickCount64());
        path_ = std::filesystem::path(root) / directory_name;
        REQUIRE(std::filesystem::create_directories(path_));
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    std::filesystem::path path_;
};

struct EnvironmentGuard final {
    EnvironmentGuard(const wchar_t* name, const std::filesystem::path& value) : name_(name) {
        const DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (required > 0) {
            previous_.resize(required);
            const DWORD written =
                GetEnvironmentVariableW(name_.c_str(), previous_.data(), required);
            REQUIRE(written > 0);
            previous_.resize(written);
            had_previous_ = true;
        }
        REQUIRE(SetEnvironmentVariableW(name_.c_str(), value.c_str()));
    }

    ~EnvironmentGuard() {
        (void)SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
    }

    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
};

struct LauncherGuard final {
    ~LauncherGuard() {
        sao_ai_editor_destroy(value);
    }
    sao_ai_editor_launcher_t value = nullptr;
};

struct ProcessGuard final {
    ~ProcessGuard() {
        if (value == nullptr) {
            return;
        }
        if (WaitForSingleObject(value, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(value, 1);
            (void)WaitForSingleObject(value, 3000);
        }
        CloseHandle(value);
    }

    HANDLE value = nullptr;
};

struct WindowSnapshot final {
    DWORD process_id = 0;
    HWND window = nullptr;
    int edit_count = 0;
    bool has_send_button = false;
    bool has_clear_button = false;
    bool has_status_bar = false;
};

BOOL CALLBACK inspect_child_window(HWND window, LPARAM parameter) {
    auto& snapshot = *reinterpret_cast<WindowSnapshot*>(parameter);
    std::array<wchar_t, 128> class_name{};
    if (GetClassNameW(window, class_name.data(),
                      static_cast<int>(class_name.size())) <= 0) {
        return TRUE;
    }
    if (_wcsicmp(class_name.data(), L"Edit") == 0) {
        ++snapshot.edit_count;
        return TRUE;
    }
    if (_wcsicmp(class_name.data(), L"Button") == 0) {
        std::array<wchar_t, 64> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        snapshot.has_send_button |= _wcsicmp(text.data(), L"Send") == 0;
        snapshot.has_clear_button |= _wcsicmp(text.data(), L"Clear") == 0;
        return TRUE;
    }
    snapshot.has_status_bar |=
        _wcsicmp(class_name.data(), L"msctls_statusbar32") == 0;
    return TRUE;
}

BOOL CALLBACK inspect_top_level_window(HWND window, LPARAM parameter) {
    auto& snapshot = *reinterpret_cast<WindowSnapshot*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != snapshot.process_id) {
        return TRUE;
    }

    std::array<wchar_t, 128> class_name{};
    if (GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size())) <= 0 ||
        std::wstring_view(class_name.data()) != L"{B6D9F274-3E15-4A82-91CF-7D48B3E5A0F6}") {
        return TRUE;
    }
    snapshot.window = window;
    EnumChildWindows(window, inspect_child_window, reinterpret_cast<LPARAM>(&snapshot));
    return FALSE;
}

WindowSnapshot wait_for_editor_window(DWORD process_id, HANDLE process) {
    const ULONGLONG deadline = GetTickCount64() + 2500U;
    WindowSnapshot snapshot{};
    do {
        snapshot = WindowSnapshot{};
        snapshot.process_id = process_id;
        EnumWindows(inspect_top_level_window,
                    reinterpret_cast<LPARAM>(&snapshot));
        if (snapshot.window != nullptr && snapshot.edit_count >= 2 &&
            snapshot.has_send_button && snapshot.has_clear_button &&
            snapshot.has_status_bar) {
            return snapshot;
        }
        if (WaitForSingleObject(process, 20) != WAIT_TIMEOUT) {
            break;
        }
    } while (GetTickCount64() < deadline);
    return snapshot;
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::wstring wide = path.native();
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string result(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), result.data(), required, nullptr,
                                nullptr) == required);
    return result;
}

Json request(sao_ai_editor_launcher_t launcher, const Json& payload) {
    const std::string text = payload.dump();
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_request(launcher, text.data(), static_cast<uint32_t>(text.size()),
                                  nullptr, 0, &required,
                                  3000) == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> response(static_cast<size_t>(required) + 1U);
    REQUIRE(sao_ai_editor_request(launcher, nullptr, 0, response.data(),
                                  static_cast<uint32_t>(response.size()), &required,
                                  3000) == SAO_AI_EDITOR_OK);
    return Json::parse(response.data(), response.data() + required);
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

DWORD run_child_and_wait(const std::filesystem::path& executable, std::wstring arguments,
                         const std::filesystem::path& working_directory) {
    std::wstring command_line = L"\"" + executable.native() + L"\" " + std::move(arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    REQUIRE(CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, working_directory.c_str(), &startup, &process_info));
    ProcessGuard process{process_info.hProcess};
    CloseHandle(process_info.hThread);
    REQUIRE(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    REQUIRE(GetExitCodeProcess(process.value, &exit_code));
    return exit_code;
}

} // namespace

TEST_CASE("production AI Editor child dispatches native runtime requests",
          "[plugins][ai_editor][production_child][integration]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"workspace with spaces";
    const auto profile = temporary.path() / L"profile";
    REQUIRE(std::filesystem::create_directories(workspace));
    REQUIRE(std::filesystem::create_directories(profile));
    EnvironmentGuard profile_guard(L"USERPROFILE", profile);

    const std::filesystem::path executable = SAO_AI_EDITOR_PRODUCTION_EXECUTABLE;
    REQUIRE(std::filesystem::is_regular_file(executable));
    const std::string executable_utf8 = path_utf8(executable);
    const std::string base_dir_utf8 = path_utf8(executable.parent_path());
    const std::string workspace_utf8 = path_utf8(workspace);
    const std::string extra_args =
        "--headless --workspace \"" + workspace_utf8 + "\"";

    SaoAiEditorLaunchConfig config{};
    config.executable_utf8 = executable_utf8.c_str();
    config.base_dir_utf8 = base_dir_utf8.c_str();
    config.extra_args_utf8 = extra_args.c_str();
    config.handshake_timeout_ms = 3000;
    config.request_timeout_ms = 3000;

    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);

    const Json edit_request{
        {"jsonrpc", "2.0"},
        {"id", 41},
        {"method", "tools.call"},
        {"params",
         {{"mode", "agent"},
          {"name", "editFile"},
          {"arguments",
           {{"path", "native-child.txt"}, {"content", "production child dispatched this"}}}}},
        {"sao", {{"protocolVersion", 1}}},
    };
    const Json response = request(launcher.value, edit_request);
    REQUIRE(response["id"] == 41);
    REQUIRE(response.contains("result"));
    REQUIRE(read_all(workspace / L"native-child.txt") == "production child dispatched this");

    const Json invalid_response = request(launcher.value, Json{{"jsonrpc", "2.0"},
                                                               {"id", 42},
                                                               {"method", "missing.method"},
                                                               {"params", Json::object()},
                                                               {"sao", {{"protocolVersion", 1}}}});
    REQUIRE(invalid_response["id"] == 42);
    REQUIRE(invalid_response["error"]["code"] == -32601);

    int32_t exit_code = -1;
    REQUIRE(sao_ai_editor_shutdown(launcher.value, 3000, &exit_code) == SAO_AI_EDITOR_OK);
    REQUIRE(exit_code == 0);

    bool running = true;
    REQUIRE(sao_ai_editor_status(launcher.value, &running, &exit_code) == SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(running);

    const auto working_directory_workspace = temporary.path() / L"working directory workspace";
    REQUIRE(std::filesystem::create_directories(working_directory_workspace));
    const std::string working_directory_utf8 = path_utf8(working_directory_workspace);
    SaoAiEditorLaunchConfig working_directory_config{};
    working_directory_config.executable_utf8 = executable_utf8.c_str();
    working_directory_config.base_dir_utf8 = working_directory_utf8.c_str();
    working_directory_config.extra_args_utf8 = "--headless";
    working_directory_config.handshake_timeout_ms = 3000;
    working_directory_config.request_timeout_ms = 3000;

    LauncherGuard working_directory_launcher;
    REQUIRE(sao_ai_editor_create(&working_directory_config, &working_directory_launcher.value) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(working_directory_launcher.value, nullptr) == SAO_AI_EDITOR_OK);
    const Json working_directory_response = request(
        working_directory_launcher.value,
        Json{{"jsonrpc", "2.0"},
             {"id", 43},
             {"method", "tools.call"},
             {"params",
              {{"mode", "agent"},
               {"name", "editFile"},
               {"arguments",
                {{"path", "working-directory.txt"}, {"content", "working directory selected"}}}}},
             {"sao", {{"protocolVersion", 1}}}});
    REQUIRE(working_directory_response["id"] == 43);
    REQUIRE(working_directory_response.contains("result"));
    REQUIRE(read_all(working_directory_workspace / L"working-directory.txt") ==
            "working directory selected");
    REQUIRE(sao_ai_editor_shutdown(working_directory_launcher.value, 3000, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(exit_code == 0);
}

TEST_CASE("AI Editor hidden NativeWindow remains a UI smoke fixture",
          "[plugins][ai_editor][production_child][ui][fixture]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"ui smoke workspace";
    const auto profile = temporary.path() / L"ui smoke profile";
    REQUIRE(std::filesystem::create_directories(workspace));
    REQUIRE(std::filesystem::create_directories(profile));
    EnvironmentGuard profile_guard(L"USERPROFILE", profile);

    const std::filesystem::path executable = SAO_AI_EDITOR_PRODUCTION_EXECUTABLE;
    REQUIRE(std::filesystem::is_regular_file(executable));
    std::wstring command_line = L"\"" + executable.native() +
                                L"\" --ui-smoke-test --hidden "
                                L"--auto-exit-ms 3000 --workspace \"" +
                                workspace.native() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    REQUIRE(CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           executable.parent_path().c_str(), &startup, &process_info));
    ProcessGuard process{process_info.hProcess};
    CloseHandle(process_info.hThread);

    (void)WaitForInputIdle(process.value, 2000);
    const WindowSnapshot snapshot = wait_for_editor_window(process_info.dwProcessId, process.value);
    REQUIRE(snapshot.window != nullptr);
    REQUIRE_FALSE(IsWindowVisible(snapshot.window));
    REQUIRE(snapshot.edit_count >= 2);
    REQUIRE(snapshot.has_send_button);
    REQUIRE(snapshot.has_clear_button);
    REQUIRE(snapshot.has_status_bar);

    REQUIRE(PostMessageW(snapshot.window, WM_CLOSE, 0, 0));
    REQUIRE(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0);
    DWORD exit_code = 1;
    REQUIRE(GetExitCodeProcess(process.value, &exit_code));
    REQUIRE(exit_code == 0);
}

TEST_CASE("production WebView mode requires the complete compositor bridge",
        "[plugins][ai_editor][production_child][webview][fail_closed]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"webview workspace";
    REQUIRE(std::filesystem::create_directories(workspace));
    EnvironmentGuard path_guard(L"PATH", temporary.path());

    const std::filesystem::path executable =
        SAO_AI_EDITOR_PRODUCTION_EXECUTABLE;
    REQUIRE(std::filesystem::is_regular_file(executable));
#if SAO_AI_EDITOR_HAS_WEBVIEW
    REQUIRE_FALSE(std::filesystem::exists(executable.parent_path() / L"WebView2Loader.dll"));
#endif
    const std::wstring base = L"--webview --workspace \"" + workspace.native() + L"\"";
    CHECK(run_child_and_wait(executable, base, temporary.path()) == 2);
    CHECK(run_child_and_wait(executable, base + L" --sao-mmf-name Local\\SaoFrame",
                             temporary.path()) == 2);
    CHECK(run_child_and_wait(executable, base + L" --sao-input-ring-name Local\\SaoInput",
                             temporary.path()) == 2);
    CHECK(run_child_and_wait(executable,
                             base + L" --hidden --sao-mmf-name Local\\SaoFrame"
                                    L" --sao-input-ring-name Local\\SaoInput",
                             temporary.path()) == 2);
    CHECK(run_child_and_wait(executable,
                             base + L" --sao-mmf-name Local\\SaoFrame"
                                    L" --sao-input-ring-name Local\\SaoInput",
                             temporary.path()) == 12);
}

TEST_CASE("shipping AI Editor excludes standalone panel entry points",
          "[plugins][ai_editor][production_child][shipping][focused]") {
    const std::string main_source = read_all(SAO_AI_EDITOR_MAIN_SOURCE);
    CHECK(main_source.find("gpu_hunt_panel_show") == std::string::npos);
    CHECK(main_source.find("--gpu-hunt") == std::string::npos);

    const std::string cmake_source = read_all(SAO_AI_EDITOR_CMAKE_SOURCE);
    CHECK(cmake_source.find("src/gpu_hunt_panel.cpp") == std::string::npos);

    const std::string webview_source = read_all(SAO_AI_EDITOR_WEBVIEW_SOURCE);
    CHECK(webview_source.find("ShowWindow(") == std::string::npos);
    CHECK(webview_source.find("WS_OVERLAPPEDWINDOW") ==
          std::string::npos);
    CHECK(webview_source.find("WS_POPUP") != std::string::npos);

    TemporaryDirectory temporary;
    const std::filesystem::path executable =
        SAO_AI_EDITOR_PRODUCTION_EXECUTABLE;
    REQUIRE(std::filesystem::is_regular_file(executable));
    CHECK(run_child_and_wait(executable, L"--gpu-hunt", temporary.path()) ==
          2);
    CHECK(run_child_and_wait(executable, LR"(--sao-ai-editor-pipe \\.\pipe\sao-visible-child)",
                             temporary.path()) == 2);
    CHECK(run_child_and_wait(
              executable,
              LR"(--sao-ai-editor-pipe \\.\pipe\sao-headless-child --headless --sao-mmf-name Local\Unexpected)",
              temporary.path()) == 2);
}
