// SAO Auto — launcher/tests/test_working_dir.cpp

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/user_menu.h"
#include "sao/launcher/working_dir.h"

#include <windows.h>
#include <iterator>
#include <string>

using namespace sao::launcher;

TEST_CASE("computeBaseDir strips filename", "[launcher][working_dir]") {
    wchar_t out[MAX_PATH]{};
    REQUIRE(computeBaseDir(L"C:\\SaoAuto\\SaoAuto.exe", out, MAX_PATH));
    REQUIRE(std::wstring{out} == L"C:\\SaoAuto");
}

TEST_CASE("computeBaseDir walks up 'runtime' layer", "[launcher][working_dir]") {
    // Onedir layout where the launcher lives under runtime/.
    wchar_t out[MAX_PATH]{};
    REQUIRE(computeBaseDir(L"C:\\SaoAuto\\runtime\\SaoAuto.exe", out, MAX_PATH));
    REQUIRE(std::wstring{out} == L"C:\\SaoAuto");
}

TEST_CASE("computeBaseDir rejects null / empty", "[launcher][working_dir]") {
    wchar_t out[MAX_PATH]{};
    REQUIRE_FALSE(computeBaseDir(nullptr, out, MAX_PATH));
    REQUIRE_FALSE(computeBaseDir(L"C:\\x.exe", nullptr, MAX_PATH));
    REQUIRE_FALSE(computeBaseDir(L"C:\\x.exe", out, 0));
}

TEST_CASE("buildUserDocsIndexPath appends the packaged docs location",
          "[launcher][working_dir][user_menu]") {
    wchar_t out[MAX_PATH]{};
    REQUIRE(buildUserDocsIndexPath(L"C:\\SaoAuto", out, MAX_PATH));
    REQUIRE(std::wstring{out} == L"C:\\SaoAuto\\docs\\html\\index.html");

    REQUIRE(buildUserDocsIndexPath(L"C:\\SaoAuto\\", out, MAX_PATH));
    REQUIRE(std::wstring{out} == L"C:\\SaoAuto\\docs\\html\\index.html");
}

TEST_CASE("buildUserDocsIndexPath rejects invalid or short output buffers",
          "[launcher][working_dir][user_menu]") {
    wchar_t out[16]{};
    REQUIRE_FALSE(buildUserDocsIndexPath(nullptr, out, std::size(out)));
    REQUIRE_FALSE(buildUserDocsIndexPath(L"", out, std::size(out)));
    REQUIRE_FALSE(buildUserDocsIndexPath(L"C:\\SaoAuto", nullptr, std::size(out)));
    REQUIRE_FALSE(buildUserDocsIndexPath(L"C:\\SaoAuto", out, 0));
    REQUIRE_FALSE(buildUserDocsIndexPath(L"C:\\SaoAuto", out, std::size(out)));
    REQUIRE(out[0] == L'\0');
}
