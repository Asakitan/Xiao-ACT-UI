// SAO Auto — launcher/tests/test_cutover_diff_phase12.cpp
//
// tools/cutover_diff deterministic-output stability check.
//
// The tool must emit byte-identical output on two consecutive runs against
// the same tree.  This test spawns the built cutover_diff twice, captures
// stdout, and compares.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef SAO_CUTOVER_DIFF_EXE_DIR
#define SAO_CUTOVER_DIFF_EXE_DIR ""
#endif
#ifndef SAO_CUTOVER_DIFF_EXE_NAME
#define SAO_CUTOVER_DIFF_EXE_NAME ""
#endif
#ifndef SAO_PYTHON_ROOT_HINT
#define SAO_PYTHON_ROOT_HINT ""
#endif
#ifndef SAO_CPP_ROOT_HINT
#define SAO_CPP_ROOT_HINT ""
#endif

namespace {

// Spawn a child process, capture stdout, return the raw bytes.  Returns
// exit code via ``exit_code_out``; return value is the captured stdout.
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
    if (!ok) {
        ::CloseHandle(read_end);
        return {};
    }

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

std::string cutover_diff_exe_path() {
    std::string dir = SAO_CUTOVER_DIFF_EXE_DIR;
    std::string nm  = SAO_CUTOVER_DIFF_EXE_NAME;
    if (dir.empty() || nm.empty()) return {};
    if (dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
    return dir + nm;
}

std::string quote(const std::string& s) {
    if (s.find(' ') == std::string::npos && s.find('"') == std::string::npos)
        return s;
    return std::string("\"") + s + "\"";
}

} // namespace

TEST_CASE("cutover_diff_json_stable_across_runs",
          "[launcher][cutover_diff][phase12]") {
    std::string exe = cutover_diff_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("cutover_diff exe not built in this configuration; skipping subprocess test");
        return;
    }
    std::string py_root = SAO_PYTHON_ROOT_HINT;
    std::string cpp_root = SAO_CPP_ROOT_HINT;
    // If either root doesn't exist, we still exercise the tool but the
    // test can't check payload details.
    REQUIRE(fs::exists(py_root));
    REQUIRE(fs::exists(cpp_root));

    std::string cmd = quote(exe) + " --python-root " + quote(py_root)
                    + " --cpp-root " + quote(cpp_root);

    DWORD ec1 = 1, ec2 = 1;
    std::string out1 = spawn_and_capture(cmd, &ec1);
    std::string out2 = spawn_and_capture(cmd, &ec2);
    REQUIRE(ec1 == 0);
    REQUIRE(ec2 == 0);
    REQUIRE_FALSE(out1.empty());
    REQUIRE(out1 == out2);

    // Sanity: schema tag + a couple of expected entries.
    REQUIRE(out1.find("\"schema\": \"sao.cutover_diff.v1\"") != std::string::npos);
    REQUIRE(out1.find("\"python_modules\"")               != std::string::npos);
    REQUIRE(out1.find("\"cpp_subsystems\"")                != std::string::npos);
    REQUIRE(out1.find("\"diff\"")                          != std::string::npos);
    // Must reference launcher subsystem.
    REQUIRE(out1.find("\"launcher\"")                      != std::string::npos);
}

TEST_CASE("cutover_diff_bad_args_returns_nonzero",
          "[launcher][cutover_diff][phase12]") {
    std::string exe = cutover_diff_exe_path();
    if (exe.empty() || !fs::exists(exe)) {
        WARN("cutover_diff exe not built in this configuration; skipping subprocess test");
        return;
    }

    DWORD ec = 0;
    std::string cmd = quote(exe) + " --unknown-flag";
    (void)spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 1);

    // Missing required arg returns 1 as well.
    ec = 0;
    cmd = quote(exe);
    (void)spawn_and_capture(cmd, &ec);
    REQUIRE(ec == 1);
}
