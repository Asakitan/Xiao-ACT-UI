#include <catch2/catch_test_macros.hpp>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_status.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#ifndef SAO_AI_EDITOR_CHILD_FIXTURE
#define SAO_AI_EDITOR_CHILD_FIXTURE ""
#endif

namespace {

struct FixtureConfig {
    std::string executable = SAO_AI_EDITOR_CHILD_FIXTURE;
    std::string base_dir = std::filesystem::path(executable).parent_path().string();
    std::string extra_args;
    std::string pipe_name;

    SaoAiEditorLaunchConfig make() const {
        SaoAiEditorLaunchConfig config{};
        config.executable_utf8 = executable.c_str();
        config.base_dir_utf8 = base_dir.c_str();
        config.extra_args_utf8 = extra_args.empty() ? nullptr : extra_args.c_str();
        config.ipc_pipe_name_utf8 = pipe_name.empty() ? nullptr : pipe_name.c_str();
        config.inherit_stdio = false;
        config.handshake_timeout_ms = 2000;
        config.request_timeout_ms = 1000;
        return config;
    }
};

struct LauncherGuard {
    sao_ai_editor_launcher_t value = nullptr;
    ~LauncherGuard() { sao_ai_editor_destroy(value); }
};

std::string request(sao_ai_editor_launcher_t launcher,
                    const std::string& payload,
                    uint32_t timeout_ms = 1000) {
    std::array<char, 256> response{};
    uint32_t length = 0;
    REQUIRE(sao_ai_editor_request(
                launcher, payload.data(), static_cast<uint32_t>(payload.size()),
                response.data(), static_cast<uint32_t>(response.size()),
                &length, timeout_ms) == SAO_AI_EDITOR_OK);
    return std::string(response.data(), length);
}

}  // namespace

TEST_CASE("ai editor launches real configured child and handshakes",
          "[ai_editor][process]") {
    FixtureConfig strings;
    strings.extra_args = "--fixture-expect \"value with spaces\"";
    const auto config = strings.make();
    LauncherGuard launcher;

    REQUIRE(sao_ai_editor_launcher_available());
    REQUIRE(sao_ai_editor_abi_version() == SAO_AI_EDITOR_ABI_VERSION);
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);

    bool running = true;
    int32_t exit_code = -1;
    REQUIRE(sao_ai_editor_status(launcher.value, &running, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(running);

    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_status(launcher.value, &running, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(running);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) ==
            SAO_AI_EDITOR_ERR_ALREADY_RUNNING);
}

TEST_CASE("ai editor real child request response preserves caller buffer ABI",
          "[ai_editor][process][ipc]") {
    FixtureConfig strings;
    const auto config = strings.make();
    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);

    const std::string payload = "ping with spaces";
    REQUIRE(request(launcher.value, payload) == "response:" + payload);

    const std::string second = "buffer-check";
    std::array<char, 2> tiny{};
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_request(
                launcher.value, second.data(), static_cast<uint32_t>(second.size()),
                tiny.data(), static_cast<uint32_t>(tiny.size()), &required, 1000) ==
            SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required == std::string("response:" + second).size());

    std::array<char, 128> response{};
    uint32_t length = 0;
    REQUIRE(sao_ai_editor_request(
                launcher.value, nullptr, 0, response.data(),
                static_cast<uint32_t>(response.size()), &length, 1000) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(std::string(response.data(), length) == "response:" + second);
}

TEST_CASE("ai editor request timeout is bounded",
          "[ai_editor][process][timeout]") {
    FixtureConfig strings;
    const auto config = strings.make();
    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);

    std::array<char, 32> response{};
    uint32_t length = 0;
    const ULONGLONG started = GetTickCount64();
    REQUIRE(sao_ai_editor_request(
                launcher.value, "timeout", 7, response.data(),
                static_cast<uint32_t>(response.size()), &length, 100) ==
            SAO_AI_EDITOR_ERR_IPC_TIMEOUT);
    REQUIRE(GetTickCount64() - started < 2000);
}

TEST_CASE("ai editor detects child exit during request",
          "[ai_editor][process][exit]") {
    FixtureConfig strings;
    const auto config = strings.make();
    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);

    std::array<char, 32> response{};
    uint32_t length = 0;
    REQUIRE(sao_ai_editor_request(
                launcher.value, "exit-now", 8, response.data(),
                static_cast<uint32_t>(response.size()), &length, 1000) ==
            SAO_AI_EDITOR_ERR_IPC_CLOSED);

    bool running = true;
    int32_t exit_code = -1;
    REQUIRE(sao_ai_editor_status(launcher.value, &running, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(running);
    REQUIRE(exit_code == 23);
}

TEST_CASE("ai editor launch timeout and early exit fail closed",
          "[ai_editor][process][handshake]") {
    SECTION("child never connects") {
        FixtureConfig strings;
        strings.extra_args = "--fixture-no-connect";
        auto config = strings.make();
        config.handshake_timeout_ms = 100;
        LauncherGuard launcher;
        REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
        REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) ==
                SAO_AI_EDITOR_ERR_IPC_TIMEOUT);
    }

    SECTION("child exits before connecting") {
        FixtureConfig strings;
        strings.extra_args = "--fixture-exit-before-connect";
        const auto config = strings.make();
        LauncherGuard launcher;
        REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
        int32_t exit_code = -1;
        REQUIRE(sao_ai_editor_launch(launcher.value, &exit_code) ==
                SAO_AI_EDITOR_ERR_IPC_CLOSED);
        REQUIRE(exit_code == 19);
    }
}

TEST_CASE("ai editor shutdown and destroy clean up child process",
          "[ai_editor][process][cleanup]") {
    FixtureConfig strings;
    const auto config = strings.make();
    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(&config, &launcher.value) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_launch(launcher.value, nullptr) == SAO_AI_EDITOR_OK);

    const DWORD pid = static_cast<DWORD>(std::stoul(request(launcher.value, "pid")));
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    REQUIRE(process != nullptr);

    int32_t exit_code = -1;
    REQUIRE(sao_ai_editor_shutdown(launcher.value, 2000, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(exit_code == 0);
    REQUIRE(WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
    CloseHandle(process);

    bool running = true;
    REQUIRE(sao_ai_editor_status(launcher.value, &running, &exit_code) ==
            SAO_AI_EDITOR_OK);
    REQUIRE_FALSE(running);
}

TEST_CASE("ai editor missing executable and base directory fail closed",
          "[ai_editor][config]") {
    LauncherGuard launcher;
    REQUIRE(sao_ai_editor_create(nullptr, &launcher.value) ==
            SAO_AI_EDITOR_ERR_CONFIG_MISSING);
    REQUIRE(launcher.value == nullptr);

    SaoAiEditorLaunchConfig missing_executable{};
    missing_executable.executable_utf8 = "Z:/missing/AiEditor.exe";
    missing_executable.base_dir_utf8 = "Z:/missing";
    REQUIRE(sao_ai_editor_create(&missing_executable, &launcher.value) ==
            SAO_AI_EDITOR_ERR_CONFIG_MISSING);
    REQUIRE(launcher.value == nullptr);

    FixtureConfig strings;
    auto missing_base = strings.make();
    missing_base.base_dir_utf8 = "Z:/missing";
    REQUIRE(sao_ai_editor_create(&missing_base, &launcher.value) ==
            SAO_AI_EDITOR_ERR_CONFIG_MISSING);
    REQUIRE(launcher.value == nullptr);
}
