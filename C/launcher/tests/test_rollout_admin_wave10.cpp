// SAO Auto - launcher/tests/test_rollout_admin_wave10.cpp
//
// Wave 10 / Agent a - Phase 12 rollout admin CLI integration test.
//
// Spawns the built sao_rollout_admin.exe with --appdata-dir pointing at a
// scratch dir, captures stdout, and checks the printed JSON.

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/rollout.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef SAO_ROLLOUT_ADMIN_EXE_DIR
#define SAO_ROLLOUT_ADMIN_EXE_DIR ""
#endif
#ifndef SAO_ROLLOUT_ADMIN_EXE_NAME
#define SAO_ROLLOUT_ADMIN_EXE_NAME ""
#endif

namespace {

std::string rollout_admin_exe_path() {
    std::string dir = SAO_ROLLOUT_ADMIN_EXE_DIR;
    std::string nm  = SAO_ROLLOUT_ADMIN_EXE_NAME;
    if (dir.empty() || nm.empty()) return {};
    if (dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
    return dir + nm;
}

std::string quote(const std::string& s) {
    if (s.find(' ') == std::string::npos && s.find('"') == std::string::npos)
        return s;
    return std::string("\"") + s + "\"";
}

std::string spawn_and_capture(const std::string& cmdline, DWORD* exit_code_out) {
    if (exit_code_out) *exit_code_out = 1;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr, write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) return {};
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError  = write_end;
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');
    BOOL ok = ::CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_end);
    if (!ok) { ::CloseHandle(read_end); return {}; }

    std::string blob;
    char buf[4096];
    DWORD read_n = 0;
    while (::ReadFile(read_end, buf, sizeof(buf), &read_n, nullptr) && read_n > 0) {
        blob.append(buf, buf + read_n);
    }
    ::CloseHandle(read_end);
    ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD ec = 1;
    ::GetExitCodeProcess(pi.hProcess, &ec);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    if (exit_code_out) *exit_code_out = ec;
    return blob;
}

fs::path unique_scratch_dir(const char* stem) {
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    wchar_t buf[MAX_PATH]{};
    _snwprintf_s(buf, MAX_PATH, _TRUNCATE,
                 L"%ssao_rolladmin_%hs_%llx",
                 tmp, stem, static_cast<unsigned long long>(li.QuadPart));
    fs::path p(buf);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string wpath_to_utf8(const fs::path& p) {
    auto w = p.wstring();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

// ===========================================================================
// 1) show_output_contains_current_percent
// ===========================================================================
TEST_CASE("rollout_admin_show_output_contains_current_percent",
          "[launcher][rollout_admin][wave10]") {
    std::string exe = rollout_admin_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("sao_rollout_admin exe not built in this configuration; skipping");
        return;
    }
    auto scratch = unique_scratch_dir("show");
    std::string scratch_utf8 = wpath_to_utf8(scratch);

    // Seed a config with cpp_percent=42.
    sao_rollout_test_set_appdata_dir(scratch.wstring().c_str());
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 42;
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);
    sao_rollout_test_set_appdata_dir(nullptr);

    DWORD ec = 0;
    std::string cmd = quote(exe) + " --appdata-dir " + quote(scratch_utf8)
                    + " --show";
    std::string out = spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 0);
    REQUIRE_FALSE(out.empty());
    REQUIRE(out.find("\"cpp_percent\": 42") != std::string::npos);
    REQUIRE(out.find("\"schema\": 1") != std::string::npos);
    REQUIRE(out.find("\"retreat_history\"") != std::string::npos);

    std::error_code fec;
    fs::remove_all(scratch, fec);
}

// ===========================================================================
// 2) set_percent_persists_and_shows_new_value
// ===========================================================================
TEST_CASE("rollout_admin_set_percent_persists_and_shows_new_value",
          "[launcher][rollout_admin][wave10]") {
    std::string exe = rollout_admin_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("sao_rollout_admin exe not built in this configuration; skipping");
        return;
    }
    auto scratch = unique_scratch_dir("setpct");
    std::string scratch_utf8 = wpath_to_utf8(scratch);

    // Set to 25 via CLI.
    DWORD ec = 0;
    std::string cmd = quote(exe) + " --appdata-dir " + quote(scratch_utf8)
                    + " --set-percent 25";
    std::string out = spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 0);
    REQUIRE(out.find("25") != std::string::npos);

    // Show and confirm.
    cmd = quote(exe) + " --appdata-dir " + quote(scratch_utf8) + " --show";
    ec = 0;
    out = spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 0);
    REQUIRE(out.find("\"cpp_percent\": 25") != std::string::npos);

    // Direct read from disk matches.
    sao_rollout_test_set_appdata_dir(scratch.wstring().c_str());
    sao_rollout_config cfg{};
    REQUIRE(sao_rollout_config_load(&cfg) == SAO_STATUS_OK);
    REQUIRE(cfg.cpp_percent == 25);
    sao_rollout_test_set_appdata_dir(nullptr);

    std::error_code fec;
    fs::remove_all(scratch, fec);
}

// ===========================================================================
// 3) simulate_buckets_10000_uniform_within_tolerance
// ===========================================================================
TEST_CASE("rollout_admin_simulate_buckets_10000_uniform_within_tolerance",
          "[launcher][rollout_admin][wave10]") {
    std::string exe = rollout_admin_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("sao_rollout_admin exe not built in this configuration; skipping");
        return;
    }
    auto scratch = unique_scratch_dir("simulate");
    std::string scratch_utf8 = wpath_to_utf8(scratch);

    DWORD ec = 0;
    std::string cmd = quote(exe) + " --appdata-dir " + quote(scratch_utf8)
                    + " --simulate-buckets --count 10000";
    std::string out = spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 0);
    REQUIRE(out.find("\"count\": 10000") != std::string::npos);
    REQUIRE(out.find("\"histogram\":") != std::string::npos);

    // Parse stddev out of the "\"stddev\": X.YYYY," line.
    auto pos = out.find("\"stddev\":");
    REQUIRE(pos != std::string::npos);
    double stddev = std::atof(out.c_str() + pos + 9);
    REQUIRE(stddev > 0.0);
    REQUIRE(stddev < 20.0);

    std::error_code fec;
    fs::remove_all(scratch, fec);
}

// ===========================================================================
// 4) reset_retreat_clears_history
// ===========================================================================
TEST_CASE("rollout_admin_reset_retreat_clears_history",
          "[launcher][rollout_admin][wave10]") {
    std::string exe = rollout_admin_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("sao_rollout_admin exe not built in this configuration; skipping");
        return;
    }
    auto scratch = unique_scratch_dir("reset");
    std::string scratch_utf8 = wpath_to_utf8(scratch);

    // Seed a config with two retreat entries.
    sao_rollout_test_set_appdata_dir(scratch.wstring().c_str());
    sao_rollout_config cfg{};
    sao_rollout_config_default(&cfg);
    cfg.cpp_percent = 25;
    cfg.retreat_history_count = 2;
    cfg.retreat_history[0] = {1'700'000'000'000LL, 100, 50, "3_of_5_failed"};
    cfg.retreat_history[1] = {1'700'000'060'000LL,  50, 25, "3_of_5_failed"};
    REQUIRE(sao_rollout_config_save(&cfg) == SAO_STATUS_OK);
    sao_rollout_test_set_appdata_dir(nullptr);

    // Reset via CLI.
    DWORD ec = 0;
    std::string cmd = quote(exe) + " --appdata-dir " + quote(scratch_utf8)
                    + " --reset-retreat";
    std::string out = spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 0);
    REQUIRE(out.find("cleared") != std::string::npos);

    // Confirm on-disk.
    sao_rollout_test_set_appdata_dir(scratch.wstring().c_str());
    sao_rollout_config after{};
    REQUIRE(sao_rollout_config_load(&after) == SAO_STATUS_OK);
    REQUIRE(after.retreat_history_count == 0);
    REQUIRE(after.cpp_percent == 25);   // percent preserved
    sao_rollout_test_set_appdata_dir(nullptr);

    std::error_code fec;
    fs::remove_all(scratch, fec);
}
