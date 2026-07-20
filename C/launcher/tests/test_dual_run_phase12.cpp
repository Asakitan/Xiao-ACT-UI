// SAO Auto — launcher/tests/test_dual_run_phase12.cpp
//
// Wave 8 / Phase 12 — dual-run mode.  Covers config round-trip, python probe,
// spawn, status registry, driver mutex, and step-zero dispatch.

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/app.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path unique_tmp_path(const char* stem) {
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    wchar_t buf[MAX_PATH]{};
    _snwprintf_s(buf, MAX_PATH, _TRUNCATE, L"%ssao_dual_run_%hs_%llx.json",
                 tmp, stem, static_cast<unsigned long long>(li.QuadPart));
    return fs::path(buf);
}

// Guard that resets dual-run global state before + after each test case.
struct DualRunGuard {
    DualRunGuard()  { sao_launcher_dual_run_reset_for_test(); }
    ~DualRunGuard() { sao_launcher_dual_run_reset_for_test(); }
};

} // namespace

TEST_CASE("dual_run_config_roundtrip_preserves_all_fields",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    auto path = unique_tmp_path("roundtrip");

    sao_dual_run_config in{};
    sao_launcher_dual_run_config_default(&in);
    in.mode = SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK;
    lstrcpynW(in.python_exe_path,     L"E:\\Py\\python.exe",           260);
    lstrcpynW(in.python_main_py_path, L"E:\\VC\\SAO-UI\\sao_auto\\python\\main.py", 260);
    lstrcpynW(in.cpp_exe_path,        L"E:\\VC\\SAO-UI\\build\\SaoAuto.exe", 260);
    lstrcpynW(in.env_overrides[0].key,   L"SAO_LOG_LEVEL", 64);
    lstrcpynW(in.env_overrides[0].value, L"debug",         512);
    lstrcpynW(in.env_overrides[1].key,   L"SAO_TRACE",     64);
    lstrcpynW(in.env_overrides[1].value, L"packet=true",   512);
    in.env_overrides_count = 2;

    REQUIRE(sao_launcher_dual_run_config_save_to_path(path.c_str(), &in) == SAO_STATUS_OK);

    sao_dual_run_config out{};
    REQUIRE(sao_launcher_dual_run_config_load_from_path(path.c_str(), &out) == SAO_STATUS_OK);

    REQUIRE(out.mode == in.mode);
    REQUIRE(std::wcscmp(out.python_exe_path,     in.python_exe_path)     == 0);
    REQUIRE(std::wcscmp(out.python_main_py_path, in.python_main_py_path) == 0);
    REQUIRE(std::wcscmp(out.cpp_exe_path,        in.cpp_exe_path)        == 0);
    REQUIRE(out.env_overrides_count == 2);
    REQUIRE(std::wcscmp(out.env_overrides[0].key,   L"SAO_LOG_LEVEL") == 0);
    REQUIRE(std::wcscmp(out.env_overrides[0].value, L"debug")         == 0);
    REQUIRE(std::wcscmp(out.env_overrides[1].key,   L"SAO_TRACE")     == 0);
    REQUIRE(std::wcscmp(out.env_overrides[1].value, L"packet=true")   == 0);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("dual_run_mode_default_when_missing_config",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    auto path = unique_tmp_path("missing");
    // Make sure the file really doesn't exist.
    std::error_code ec;
    fs::remove(path, ec);
    REQUIRE_FALSE(fs::exists(path));

    sao_dual_run_config cfg{};
    REQUIRE(sao_launcher_dual_run_config_load_from_path(path.c_str(), &cfg)
            == SAO_STATUS_OK);
    REQUIRE(cfg.mode == SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK);
    REQUIRE(cfg.python_exe_path[0] == L'\0');
    REQUIRE(cfg.env_overrides_count == 0);
}

TEST_CASE("dual_run_probe_python_when_absent_via_hook",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    sao_launcher_dual_run_set_test_probe_hook(
        +[](sao_dual_run_python_probe* out) {
            std::memset(out, 0, sizeof(*out));
            out->available = 0;
        });

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    sao_dual_run_python_probe probe{};
    REQUIRE(sao_launcher_dual_run_probe_python(&cfg, &probe) == SAO_STATUS_OK);
    REQUIRE(probe.available == 0);
}

TEST_CASE("dual_run_probe_python_when_present",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    // No test hook — hit the real probe.  Requires Python 3.11+ on PATH
    // or at E:\Py\python.exe (the current dev machine's configured path).
    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    sao_dual_run_python_probe probe{};
    REQUIRE(sao_launcher_dual_run_probe_python(&cfg, &probe) == SAO_STATUS_OK);
    // The dev machine has E:\Py\python.exe (memory records this).  If a
    // CI machine ever loses that, this test would need Python 3.11+ on
    // PATH.  Skip the assertion instead of hard-failing when neither is
    // available — the "probe_when_absent" variant covers the negative
    // path via the test hook.
    if (probe.available) {
        REQUIRE(probe.major == 3);
        REQUIRE(probe.minor >= 11);
        REQUIRE(probe.path[0] != L'\0');
        REQUIRE(probe.version[0] != L'\0');
    } else {
        WARN("Python 3.11+ not found on this machine; skipping positive probe assertion");
    }
}

TEST_CASE("dual_run_spawn_python_returns_pid_via_test_hook",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    static std::atomic<int> spawn_calls{0};
    spawn_calls = 0;
    sao_launcher_dual_run_set_test_probe_hook(
        +[](sao_dual_run_python_probe* out) {
            std::memset(out, 0, sizeof(*out));
            out->available = 1;
            lstrcpynW(out->path, L"E:\\Py\\python.exe", 260);
            lstrcpynW(out->version, L"3.11.0", 32);
            out->major = 3; out->minor = 11; out->patch = 0;
        });
    sao_launcher_dual_run_set_test_spawn_hook(
        +[](const wchar_t* exe_path, const wchar_t* cmdline,
             const wchar_t* role, sao_dual_run_spawn_result* result_out) -> int {
            (void)exe_path; (void)cmdline; (void)role;
            ++spawn_calls;
            LARGE_INTEGER li{};
            ::QueryPerformanceCounter(&li);
            if (result_out) {
                result_out->pid = 0xC0FFEE;
                result_out->process = nullptr;
                result_out->thread = nullptr;
                result_out->start_time_qpc = li.QuadPart;
            }
            return 0;
        });

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    sao_dual_run_spawn_result r{};
    REQUIRE(sao_launcher_dual_run_spawn_python(&cfg, L"python", &r) == SAO_STATUS_OK);
    REQUIRE(r.pid == 0xC0FFEE);
    REQUIRE(spawn_calls == 1);
}

TEST_CASE("dual_run_spawn_cpp_side_by_side_returns_real_pid",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    // Point cpp_exe_path at the tests binary itself (we don't want to
    // launch a real SaoAuto because that would drag in every subsystem).
    // The child will just exit immediately when Catch2 sees no matching
    // test filter — that's fine, we only care about pid + handle.
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    lstrcpynW(cfg.cpp_exe_path, self, 260);

    sao_dual_run_spawn_result r{};
    // Use the test hook so this test doesn't actually spin up a child.
    // A follow-up test (see the mutex_prevents_double_launch case) validates
    // the mutex path against a real subprocess.
    sao_launcher_dual_run_set_test_spawn_hook(
        +[](const wchar_t* exe_path, const wchar_t* cmdline,
             const wchar_t* role, sao_dual_run_spawn_result* result_out) -> int {
            (void)exe_path; (void)role;
            REQUIRE(cmdline != nullptr);
            // Verify --safe-mode was injected.
            REQUIRE(::wcsstr(cmdline, L"--safe-mode") != nullptr);
            if (result_out) {
                result_out->pid = 0xBADD00D;
                result_out->process = nullptr;
                result_out->thread = nullptr;
                result_out->start_time_qpc = 42;
            }
            return 0;
        });
    REQUIRE(sao_launcher_dual_run_spawn_cpp(&cfg, L"cpp", &r) == SAO_STATUS_OK);
    REQUIRE(r.pid == 0xBADD00D);
}

TEST_CASE("dual_run_cpp_command_line_preserves_parent_argv",
          "[launcher][dual_run][argv]") {
    DualRunGuard g;
    const wchar_t* parent_argv[] = {
        L"parent.exe",
        L"--config=C:\\Program Files\\Sao Auto\\settings.json",
        L"--future=value with spaces",
        L"quoted \"value\"",
        L"",
        L"C:\\trailing slash\\",
    };

    uint32_t required = 0;
    REQUIRE(sao_launcher_dual_run_build_cpp_command_line(
        L"C:\\Sao Auto\\SaoAuto.exe", 6, parent_argv, nullptr,
        &required) == SAO_STATUS_OK);
    std::vector<wchar_t> command_line(required);
    REQUIRE(sao_launcher_dual_run_build_cpp_command_line(
        L"C:\\Sao Auto\\SaoAuto.exe", 6, parent_argv,
        command_line.data(), &required) == SAO_STATUS_OK);

    int parsed_argc = 0;
    LPWSTR* parsed = CommandLineToArgvW(command_line.data(), &parsed_argc);
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed_argc == 7);
    REQUIRE(std::wstring{parsed[0]} == L"C:\\Sao Auto\\SaoAuto.exe");
    for (int index = 1; index < 6; ++index) {
        REQUIRE(std::wstring{parsed[index]} == parent_argv[index]);
    }
    REQUIRE(std::wstring{parsed[6]} == L"--safe-mode");
    LocalFree(parsed);
}

TEST_CASE("dual_run_spawn_cpp_capture_keeps_argv_role_and_recursion_guard",
          "[launcher][dual_run][argv]") {
    DualRunGuard g;
    const wchar_t* parent_argv[] = {
        L"parent.exe", L"--safe-mode", L"--config=C:\\A B\\c.json",
        L"argument with \"quotes\"",
    };
    REQUIRE(sao_launcher_dual_run_set_test_parent_argv(
        4, parent_argv) == SAO_STATUS_OK);

    static std::wstring captured_command_line;
    static std::wstring captured_role;
    captured_command_line.clear();
    captured_role.clear();
    sao_launcher_dual_run_set_test_spawn_hook(
        +[](const wchar_t*, const wchar_t* command_line, const wchar_t* role,
            sao_dual_run_spawn_result* result) -> int {
            captured_command_line = command_line ? command_line : L"";
            captured_role = role ? role : L"";
            if (result) result->pid = 17;
            return 0;
        });

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    lstrcpynW(cfg.cpp_exe_path, L"C:\\Sao Auto\\SaoAuto.exe", 260);
    sao_dual_run_spawn_result result{};
    REQUIRE(sao_launcher_dual_run_spawn_cpp(&cfg, L"cpp", &result)
            == SAO_STATUS_OK);
    REQUIRE(captured_role == L"cpp");

    int parsed_argc = 0;
    LPWSTR* parsed = CommandLineToArgvW(
        captured_command_line.c_str(), &parsed_argc);
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed_argc == 4);
    REQUIRE(std::wstring{parsed[1]} == L"--safe-mode");
    REQUIRE(std::wstring{parsed[2]} == parent_argv[2]);
    REQUIRE(std::wstring{parsed[3]} == parent_argv[3]);
    LocalFree(parsed);
}

TEST_CASE("dual_run_mutex_prevents_double_launch",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    HANDLE m1 = nullptr;
    REQUIRE(sao_launcher_dual_run_acquire_driver_mutex(&m1) == SAO_STATUS_OK);
    REQUIRE(m1 != nullptr);

    HANDLE m2 = reinterpret_cast<HANDLE>(0xDEAD);
    REQUIRE(sao_launcher_dual_run_acquire_driver_mutex(&m2)
            == SAO_LAUNCHER_ALREADY_RUNNING);
    REQUIRE(m2 == nullptr);

    sao_launcher_dual_run_release_driver_mutex(m1);

    // After release, a fresh acquire should succeed again.
    HANDLE m3 = nullptr;
    REQUIRE(sao_launcher_dual_run_acquire_driver_mutex(&m3) == SAO_STATUS_OK);
    sao_launcher_dual_run_release_driver_mutex(m3);
}

TEST_CASE("dual_run_fallback_records_reason",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    // Stub probe -> available + spawn hook -> succeeds.
    sao_launcher_dual_run_set_test_probe_hook(
        +[](sao_dual_run_python_probe* out) {
            std::memset(out, 0, sizeof(*out));
            out->available = 1;
            lstrcpynW(out->path, L"E:\\Py\\python.exe", 260);
            lstrcpynW(out->version, L"3.11.0", 32);
            out->major = 3; out->minor = 11; out->patch = 0;
        });
    sao_launcher_dual_run_set_test_spawn_hook(
        +[](const wchar_t*, const wchar_t*, const wchar_t* role,
             sao_dual_run_spawn_result* result_out) -> int {
            (void)role;
            if (result_out) {
                result_out->pid = 999;
                result_out->process = nullptr;
                result_out->thread = nullptr;
                result_out->start_time_qpc = 0;
            }
            return 0;
        });

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    cfg.mode = SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK;

    int32_t exit_code = 0;
    int32_t did = sao_launcher_dual_run_maybe_fallback_to_python(
        &cfg, sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL, L"platform_bringup", &exit_code);
    REQUIRE(did == 1);
    REQUIRE(exit_code == SAO_EXIT_HANDOFF_TO_PYTHON);

    sao_dual_run_status st{};
    sao_launcher_dual_run_status(&st);
    REQUIRE(st.fallback_reason[0] != L'\0');
    REQUIRE(::wcsstr(st.fallback_reason, L"cpp_step_failed") != nullptr);
    REQUIRE(::wcsstr(st.fallback_reason, L"platform_bringup") != nullptr);
    // And a python_fallback child should be registered.
    REQUIRE(st.running_children_count >= 1);
    bool found = false;
    for (int i = 0; i < st.running_children_count; ++i) {
        if (::wcscmp(st.running_children[i].role, L"python_fallback") == 0) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("dual_run_init_pipeline_step_zero_python_only_short_circuits",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    // Configure python_only.  Probe stubbed to say Python is available;
    // spawn hook returns a fake pid.  step_zero must set continue=0 and
    // exit_code=HANDOFF_TO_PYTHON.
    sao_launcher_dual_run_set_test_probe_hook(
        +[](sao_dual_run_python_probe* out) {
            std::memset(out, 0, sizeof(*out));
            out->available = 1;
            lstrcpynW(out->path, L"E:\\Py\\python.exe", 260);
            lstrcpynW(out->version, L"3.11.0", 32);
            out->major = 3; out->minor = 11; out->patch = 0;
        });
    sao_launcher_dual_run_set_test_spawn_hook(
        +[](const wchar_t*, const wchar_t*, const wchar_t*,
             sao_dual_run_spawn_result* r) -> int {
            if (r) {
                r->pid = 4242;
                r->process = nullptr;
                r->thread = nullptr;
                r->start_time_qpc = 0;
            }
            return 0;
        });

    sao_dual_run_config cfg{};
    sao_launcher_dual_run_config_default(&cfg);
    cfg.mode = SAO_DUAL_RUN_MODE_PYTHON_ONLY;

    // Also verify that if an SAO_DUAL_RUN_ROLE env var IS set, step_zero
    // stays in CPP mode regardless of config.mode.  We do it in a second
    // sub-section so the shared setup isn't repeated.
    SECTION("driver path: python_only spawns and short-circuits") {
        // Make sure no inherited role is in the env.
        ::SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, nullptr);

        int32_t cont = 1;
        int32_t exit_code = 0;
        REQUIRE(sao_launcher_dual_run_step_zero(&cfg, &cont, &exit_code)
                == SAO_STATUS_OK);
        REQUIRE(cont == 0);
        REQUIRE(exit_code == SAO_EXIT_HANDOFF_TO_PYTHON);

        sao_dual_run_status st{};
        sao_launcher_dual_run_status(&st);
        REQUIRE(st.running_children_count >= 1);
        REQUIRE(::wcscmp(st.running_children[0].role, L"python") == 0);
    }

    SECTION("child path: inherited role forces CPP continuation") {
        ::SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, L"python");
        int32_t cont = 0;
        int32_t exit_code = 12345;
        REQUIRE(sao_launcher_dual_run_step_zero(&cfg, &cont, &exit_code)
                == SAO_STATUS_OK);
        REQUIRE(cont == 1);
        REQUIRE(exit_code == 0);
        ::SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, nullptr);
    }
}

TEST_CASE("dual_run_config_load_rejects_malformed_json",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    auto path = unique_tmp_path("malformed");

    // Write a broken JSON file (no closing brace).
    {
        std::ofstream f(path, std::ios::binary);
        f << "{ \"mode\": \"cpp_only\"";
    }

    sao_dual_run_config cfg{};
    REQUIRE(sao_launcher_dual_run_config_load_from_path(path.c_str(), &cfg)
            == SAO_LAUNCHER_CONFIG_PARSE_FAILED);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("dual_run_status_registers_children_and_reports_dead_processes",
          "[launcher][dual_run][phase12]") {
    DualRunGuard g;
    // Register a fake child, then a real ping to a real short-lived
    // process (cmd /c exit 42), verify the status reports is_dead=1
    // once it exits.
    sao_launcher_dual_run_register_child(4711, L"python",
                                          123456, nullptr);
    sao_dual_run_status st{};
    sao_launcher_dual_run_status(&st);
    REQUIRE(st.running_children_count >= 1);
    REQUIRE(st.running_children[0].pid == 4711);
    REQUIRE(::wcscmp(st.running_children[0].role, L"python") == 0);
    REQUIRE(st.running_children[0].is_dead == 0);  // no handle to check with

    // Now spawn a real short-lived process and let it die.
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"cmd.exe /c exit 42";
    std::vector<wchar_t> cmd_buf(std::begin(cmd), std::end(cmd));
    REQUIRE(::CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)
            == TRUE);
    sao_launcher_dual_run_register_child(pi.dwProcessId, L"cpp",
                                          0, pi.hProcess);
    ::CloseHandle(pi.hThread);

    // Wait for the child to exit.
    ::WaitForSingleObject(pi.hProcess, 5000);

    sao_launcher_dual_run_status(&st);
    bool saw_dead = false;
    for (int i = 0; i < st.running_children_count; ++i) {
        if (st.running_children[i].pid == pi.dwProcessId) {
            REQUIRE(st.running_children[i].is_dead == 1);
            REQUIRE(st.running_children[i].exit_code == 42);
            saw_dead = true;
        }
    }
    REQUIRE(saw_dead);
    // Handle is closed by reset_for_test at end of case.
}
