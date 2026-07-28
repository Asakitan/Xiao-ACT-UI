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
          "[launcher][args]") {
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

TEST_CASE("RT I/O operator flags parse independently from smoke",
          "[launcher][args][rt_io_operator]") {
    AppState state{};
    bool exit_flag = true;
    int code = -1;
    const wchar_t* args[] = {
        L"SaoAuto.exe",
        L"--rt-io-operator",
        L"--rt-io-input-checks",
        L"--rt-io-r5-check",
        L"--rt-io-mf-check",
        L"--rt-io-exit-after-validation",
    };
    REQUIRE(parseCommandLineFromArgv(
        6, const_cast<wchar_t* const*>(args), state, exit_flag, code));
    CHECK(state.rt_io_operator);
    CHECK_FALSE(state.rt_io_preflight_only);
    CHECK(state.rt_io_input_checks);
    CHECK(state.rt_io_r5_check);
    CHECK(state.rt_io_mf_check);
    CHECK(state.rt_io_exit_after_validation);
    CHECK_FALSE(state.smoke_mode);
    CHECK_FALSE(state.exit_after_init);
    CHECK_FALSE(exit_flag);
}

TEST_CASE("RT I/O subordinate flags imply operator but never smoke",
          "[launcher][args][rt_io_operator]") {
    const wchar_t* flags[] = {
        L"--rt-io-preflight-only",
        L"--rt-io-input-checks",
        L"--rt-io-r5-check",
        L"--rt-io-mf-check",
        L"--rt-io-exit-after-validation",
    };
    for (const wchar_t* flag : flags) {
        CAPTURE(flag);
        AppState state{};
        bool exit_flag = false;
        int code = -1;
        wchar_t* args[] = {
            const_cast<wchar_t*>(L"SaoAuto.exe"),
            const_cast<wchar_t*>(flag),
        };
        REQUIRE(parseCommandLineFromArgv(
            2, args, state, exit_flag, code));
        CHECK(state.rt_io_operator);
        CHECK_FALSE(state.smoke_mode);
        CHECK_FALSE(state.exit_after_init);
    }
}

TEST_CASE("ordinary smoke flags do not enable RT I/O operator",
          "[launcher][args][rt_io_operator][smoke]") {
    AppState state{};
    bool exit_flag = false;
    int code = -1;
    const wchar_t* args[] = {
        L"SaoAuto.exe", L"--smoke", L"--exit-after-init",
    };
    REQUIRE(parseCommandLineFromArgv(
        3, const_cast<wchar_t* const*>(args), state, exit_flag, code));
    CHECK(state.smoke_mode);
    CHECK(state.exit_after_init);
    CHECK_FALSE(state.rt_io_operator);
    CHECK_FALSE(state.rt_io_preflight_only);
    CHECK_FALSE(state.rt_io_input_checks);
    CHECK_FALSE(state.rt_io_r5_check);
    CHECK_FALSE(state.rt_io_mf_check);
    CHECK_FALSE(state.rt_io_exit_after_validation);
    CHECK_FALSE(state.rt_io_force_status_page);
}

TEST_CASE("RT I/O status-page override parses independently from operator",
          "[launcher][args][rt_io_operator][status_page]") {
    AppState with_operator{};
    bool exit_flag = false;
    int code = -1;
    const wchar_t* with_operator_args[] = {
        L"SaoAuto.exe", L"--rt-io-operator", L"--rt-io-status-page",
    };
    REQUIRE(parseCommandLineFromArgv(
        3, const_cast<wchar_t* const*>(with_operator_args), with_operator,
        exit_flag, code));
    CHECK(with_operator.rt_io_operator);
    CHECK(with_operator.rt_io_force_status_page);

    // The override flag alone never implies operator mode.
    AppState alone{};
    const wchar_t* alone_args[] = {L"SaoAuto.exe", L"--rt-io-status-page"};
    REQUIRE(parseCommandLineFromArgv(
        2, const_cast<wchar_t* const*>(alone_args), alone, exit_flag, code));
    CHECK_FALSE(alone.rt_io_operator);
    CHECK(alone.rt_io_force_status_page);
}
