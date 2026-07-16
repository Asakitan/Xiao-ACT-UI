// SAO Auto — launcher/tests/test_args.cpp

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/app.h"
#include "sao/launcher/args.h"

#include <windows.h>

using namespace sao::launcher;

namespace {

// Build a wchar_t*[] from a null-terminated list of literals, safely.
struct Argv {
    wchar_t* v[16] = {};
    int      c     = 0;

    template<size_t N>
    Argv(const wchar_t* const (&items)[N]) {
        static_assert(N < 16);
        for (size_t i = 0; i < N; ++i) {
            v[i] = const_cast<wchar_t*>(items[i]);
            ++c;
        }
    }
};

} // namespace

// The test name intentionally avoids a leading '--' because
// catch_discover_tests hands the raw name to the Catch2 executable as an
// argv token, and Catch2 v3.5.x's CLI parser rejects unknown '--'
// tokens.  The assertion body (which is the actual "test logic") is
// untouched.
TEST_CASE("flag safe-mode sets AppState safe_mode", "[launcher][args]") {
    AppState s{};
    bool exit_flag = true;
    int  code      = -1;
    const wchar_t* args[] = { L"SaoAuto.exe", L"--safe-mode" };
    REQUIRE(parseCommandLineFromArgv(2, const_cast<wchar_t* const*>(args), s, exit_flag, code));
    REQUIRE(s.safe_mode == true);
    REQUIRE(s.no_license == false);
    REQUIRE(exit_flag == false);
}

TEST_CASE("flag config populates config_path", "[launcher][args]") {
    AppState s{};
    bool exit_flag = true;
    int  code      = -1;
    const wchar_t* args[] = { L"SaoAuto.exe", L"--config=C:\\my.toml" };
    REQUIRE(parseCommandLineFromArgv(2, const_cast<wchar_t* const*>(args), s, exit_flag, code));
    REQUIRE(std::wstring{s.config_path} == L"C:\\my.toml");
}

TEST_CASE("flag log-level populates log_level", "[launcher][args]") {
    AppState s{};
    bool exit_flag = true;
    int  code      = -1;
    const wchar_t* args[] = { L"SaoAuto.exe", L"--log-level=trace" };
    REQUIRE(parseCommandLineFromArgv(2, const_cast<wchar_t* const*>(args), s, exit_flag, code));
    REQUIRE(std::wstring{s.log_level} == L"trace");
}

TEST_CASE("log-level is normalized and invalid values fail closed",
          "[launcher][args][w18]") {
    AppState normalized{};
    bool exit_flag = false;
    int code = -1;
    const wchar_t* upper_args[] = {L"SaoAuto.exe", L"--log-level=DeBuG"};
    REQUIRE(parseCommandLineFromArgv(
        2, const_cast<wchar_t* const*>(upper_args), normalized, exit_flag,
        code));
    REQUIRE(std::wstring{normalized.log_level} == L"debug");

    AppState rejected{};
    const wchar_t* invalid_args[] = {L"SaoAuto.exe", L"--log-level=verbose"};
    REQUIRE_FALSE(parseCommandLineFromArgv(
        2, const_cast<wchar_t* const*>(invalid_args), rejected, exit_flag,
        code));
    REQUIRE(code == SAO_EXIT_BAD_ARGS);
}

TEST_CASE("unknown args are rejected", "[launcher][args]") {
    AppState s{};
    bool exit_flag = false;
    int  code      = -1;
    const wchar_t* args[] = { L"SaoAuto.exe", L"--bogus" };
    REQUIRE_FALSE(parseCommandLineFromArgv(2, const_cast<wchar_t* const*>(args), s, exit_flag, code));
    REQUIRE(code == SAO_EXIT_BAD_ARGS);
}
