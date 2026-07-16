// Wave 7 / Agent d tests for the Phase 9 legacy_webview compatibility
// stub.  Three test cases prove the frozen contract:
//
//   * legacy_webview_declared_frozen    — available() returns false.
//   * legacy_webview_probe_python_side  — probe() enumerates the
//     tracked Python-side references (present/missing lines).
//   * legacy_webview_manifest_exists    — manifest_path() returns
//     the docs relative path; running the test from a repo-rooted
//     working directory proves the manifest file is readable.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/legacy_webview.h"
#include "sao/core/status.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Walk up from the current working directory looking for a docs/
// sibling that contains legacy_webview_manifest.md.  CTest can be
// invoked from build/, from the source root, or from anywhere in
// between depending on the runner.
fs::path find_repo_root_with_manifest() {
    auto here = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        const auto candidate = here / "docs" / "legacy_webview_manifest.md";
        if (fs::exists(candidate)) {
            return here;
        }
        if (!here.has_parent_path() || here.parent_path() == here) break;
        here = here.parent_path();
    }
    return {};
}

}  // namespace

TEST_CASE("legacy_webview_declared_frozen", "[ui][legacy_webview][wave7]") {
    // The compatibility surface must NEVER report itself as available
    // until Phase 9 is unfrozen.  Flipping this to true without also
    // implementing every method in docs/legacy_webview_manifest.md is
    // a broken change.
    REQUIRE(sao_ui_legacy_webview_available() == false);
}

TEST_CASE("legacy_webview_probe_python_side", "[ui][legacy_webview][wave7]") {
    SECTION("probe with no python path still reports freeze status") {
        char buf[512]{};
        const int32_t status =
            sao_ui_legacy_webview_probe(nullptr, buf, sizeof(buf));
        REQUIRE(status == SAO_STATUS_OK);
        REQUIRE(std::strstr(buf, "FROZEN") != nullptr);
        REQUIRE(std::strstr(buf, "<not provided>") != nullptr);
    }

    SECTION("probe reports invalid args for a null buffer") {
        const int32_t status =
            sao_ui_legacy_webview_probe("does/not/matter", nullptr, 0);
        REQUIRE(status == SAO_STATUS_ERR_INVALID_ARGUMENT);
    }

    SECTION("probe reports too-small buffer") {
        char tiny[8]{};
        const int32_t status =
            sao_ui_legacy_webview_probe(nullptr, tiny, sizeof(tiny));
        REQUIRE(status == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    }

    SECTION("probe enumerates present sentinel files when Python tree exists") {
        // Locate the real python tree in the repo.  If the test runs
        // from a build tree that doesn't ship the sources, this
        // section becomes a no-op and we skip the enumeration.
        const auto root = find_repo_root_with_manifest();
        if (root.empty()) {
            // Repo not reachable from CWD — probe path still resolves
            // to the freeze header at minimum.
            char buf[1024]{};
            const int32_t status = sao_ui_legacy_webview_probe(
                "definitely/not/a/real/path", buf, sizeof(buf));
            REQUIRE(status == SAO_STATUS_OK);
            REQUIRE(std::strstr(buf, "FROZEN") != nullptr);
            REQUIRE(std::strstr(buf, "[missing]") != nullptr);
            return;
        }

        // Python side lives at <repo_root>/../python for the C++ tree
        // layout (sao_auto/C is a sibling of sao_auto/python).
        const auto python_side = root.parent_path() / "python";
        if (!fs::exists(python_side / "sao_webview.py")) {
            // Different tree layout — still valid, we just skip the
            // enumeration assertion.
            SUCCEED("python tree unavailable at expected sibling path");
            return;
        }

        char buf[2048]{};
        const std::string py_utf8 = python_side.string();
        const int32_t status =
            sao_ui_legacy_webview_probe(py_utf8.c_str(), buf, sizeof(buf));
        REQUIRE(status == SAO_STATUS_OK);
        REQUIRE(std::strstr(buf, "FROZEN") != nullptr);
        REQUIRE(std::strstr(buf, "sao_webview.py") != nullptr);
        REQUIRE(std::strstr(buf, "[present]") != nullptr);
        REQUIRE(std::strstr(buf, "summary:") != nullptr);
    }
}

TEST_CASE("legacy_webview_manifest_exists", "[ui][legacy_webview][wave7]") {
    SECTION("manifest_path returns the stable docs relative path") {
        char buf[256]{};
        const int32_t status =
            sao_ui_legacy_webview_manifest_path(buf, sizeof(buf));
        REQUIRE(status == SAO_STATUS_OK);
        REQUIRE(std::string(buf) == "docs/legacy_webview_manifest.md");
    }

    SECTION("manifest_path reports invalid args on null buffer") {
        const int32_t status =
            sao_ui_legacy_webview_manifest_path(nullptr, 0);
        REQUIRE(status == SAO_STATUS_ERR_INVALID_ARGUMENT);
    }

    SECTION("manifest_path reports too-small buffer") {
        char tiny[4]{};
        const int32_t status =
            sao_ui_legacy_webview_manifest_path(tiny, sizeof(tiny));
        REQUIRE(status == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    }

    SECTION("manifest file is actually readable when repo root reachable") {
        const auto root = find_repo_root_with_manifest();
        if (root.empty()) {
            SUCCEED("repo root not reachable from CWD; skipping IO check");
            return;
        }
        const auto path = root / "docs" / "legacy_webview_manifest.md";
        REQUIRE(fs::exists(path));

        // Skim the file for the FROZEN marker.  This proves the
        // manifest actually documents the freeze, not just an empty
        // placeholder.
        std::ifstream in(path);
        REQUIRE(in.is_open());
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        REQUIRE(content.find("FROZEN") != std::string::npos);
        REQUIRE(content.find("sao_webview.py") != std::string::npos);
    }
}
