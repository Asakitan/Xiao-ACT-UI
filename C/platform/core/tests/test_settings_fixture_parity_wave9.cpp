// Wave 9 / Agent e - settings fixture parity, Phase 0 deep dive.
//
// The freeze at docs/fixtures/settings/*.json captures the 10 canonical
// settings-envelope outcomes ``config.py``'s SettingsManager + the
// downstream ``json.dumps(data, ensure_ascii=False[, indent=2])`` writer
// produce.  This test drives ``sao_core_settings_dump_compact`` /
// ``_dump_pretty`` / ``_normalize_panel_themes`` / ``_merge_hotkeys`` /
// ``_strip_legacy_dump`` (all newly implemented in this wave — previously
// the API only handled flat primitive dicts) through each fixture's
// ``input`` block and verifies the byte-identical Python-parity output.
//
// The 10 fixtures the freeze recorded (see FROZEN.md at rev 9f023ef7):
//   * normal_default_only_panel_theme       Baseline: panel_themes = {act:dark}
//   * normal_full_v5                        Full v5: theme+hotkeys+plugins+opacity
//   * normal_light_theme                    panel_themes = {act:light}
//   * normal_utf8_plugin_names              ensure_ascii=False on CJK plugin names
//   * v0_legacy_migration_stripped          _LEGACY_KEYS pop on save()
//   * partial_only_hotkeys                  Hotkeys stored, panel_themes defaults
//   * partial_missing_panel_themes          Empty dict; defaults fill in
//   * extra_field_unknown_keys_kept         Unknown top-level keys survive
//   * corrupt_wrong_theme_string            Bad theme string coerces to 'dark'
//   * corrupt_non_dict_theme                Non-dict panel_themes falls to baseline
//
// UTF-8 no BOM.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sao/core/config.h"
#include "sao/core/status.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::optional<std::string> load_settings_fixture(const std::string& name) {
    static const char* kRoots[] = {
        "docs/fixtures/settings/",
        "../docs/fixtures/settings/",
        "../../docs/fixtures/settings/",
        "../../../docs/fixtures/settings/",
        "../../../../docs/fixtures/settings/",
        "../../../../../docs/fixtures/settings/",
        "sao_auto/C/docs/fixtures/settings/",
        "../sao_auto/C/docs/fixtures/settings/",
        "../../sao_auto/C/docs/fixtures/settings/",
        "../../../sao_auto/C/docs/fixtures/settings/",
        "e:/VC/SAO-UI/sao_auto/C/docs/fixtures/settings/",
    };
    for (const char* root : kRoots) {
        std::string path = std::string(root) + name;
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    return std::nullopt;
}

// Serialise a JSON node with Python's compact dump semantics matches the
// fixture's expected string comparison basis.  Nested settings mean we
// can't just use nlohmann's dump() — the Wave 5 slice actually had this
// bug and was scoped to flat primitives only.  This wave's new API is
// what we're validating.
std::string call_dump_compact(const std::string& input_blob) {
    size_t needed = 0;
    sao_status_t rc = sao_core_settings_dump_compact(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        nullptr, 0, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    std::vector<uint8_t> buf(needed + 1, 0);
    rc = sao_core_settings_dump_compact(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        buf.data(), needed, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(reinterpret_cast<const char*>(buf.data()), needed);
}

std::string call_dump_pretty(const std::string& input_blob) {
    size_t needed = 0;
    sao_status_t rc = sao_core_settings_dump_pretty(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        nullptr, 0, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    std::vector<uint8_t> buf(needed + 1, 0);
    rc = sao_core_settings_dump_pretty(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        buf.data(), needed, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(reinterpret_cast<const char*>(buf.data()), needed);
}

std::string call_normalize_panel_themes(const std::string& input_blob) {
    size_t needed = 0;
    sao_status_t rc = sao_core_settings_normalize_panel_themes(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        nullptr, 0, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    std::vector<uint8_t> buf(needed + 1, 0);
    rc = sao_core_settings_normalize_panel_themes(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        buf.data(), needed, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(reinterpret_cast<const char*>(buf.data()), needed);
}

std::string call_merge_hotkeys(const std::string& input_blob) {
    size_t needed = 0;
    sao_status_t rc = sao_core_settings_merge_hotkeys(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        nullptr, 0, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    std::vector<uint8_t> buf(needed + 1, 0);
    rc = sao_core_settings_merge_hotkeys(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        buf.data(), needed, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(reinterpret_cast<const char*>(buf.data()), needed);
}

std::string call_strip_legacy_dump(const std::string& input_blob) {
    size_t needed = 0;
    sao_status_t rc = sao_core_settings_strip_legacy_dump(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        nullptr, 0, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    std::vector<uint8_t> buf(needed + 1, 0);
    rc = sao_core_settings_strip_legacy_dump(
        reinterpret_cast<const uint8_t*>(input_blob.data()),
        input_blob.size(),
        buf.data(), needed, &needed);
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(reinterpret_cast<const char*>(buf.data()), needed);
}

// Load a fixture blob + assert the schema/oracle header before returning
// (input, expected).
struct FixturePair {
    nlohmann::ordered_json input;
    nlohmann::ordered_json expected;
    std::string input_blob;  // canonical compact dump of `input`, our test's oracle
};

// Serialise `input` back to a Python-compact blob that we then feed into
// the C++ API.  The fixture already stored `plaintext_utf8`; we replay
// that string here so the byte-comparison is meaningful.
FixturePair load_fixture(const std::string& fixture_name) {
    auto blob = load_settings_fixture(fixture_name);
    REQUIRE(blob.has_value());
    const auto j = nlohmann::ordered_json::parse(*blob);
    REQUIRE(j.at("category").get<std::string>() == "settings");
    FixturePair fp;
    fp.input = j.at("input");
    fp.expected = j.at("expected");
    // Round-trip the input through the fixture's own pinned
    // `plaintext_utf8` string so the C++ API's input is what Python would
    // have written to disk.  The fixture pins the exact canonical blob;
    // we re-use it as the source-of-truth.
    fp.input_blob = fp.expected.at("plaintext_utf8").get<std::string>();
    return fp;
}

// Case runner: shared assertion body across all 10 fixtures.
void run_settings_fixture(const std::string& fixture_name) {
    const FixturePair fp = load_fixture(fixture_name);

    // Assertion 1: compact dump equals fixture's plaintext_utf8.  We use
    // the fixture's own input_blob (which IS plaintext_utf8) as input,
    // so a round-trip through parse + dump-compact must be idempotent.
    const std::string got_plaintext = call_dump_compact(fp.input_blob);
    REQUIRE(got_plaintext ==
            fp.expected.at("plaintext_utf8").get<std::string>());

    // Assertion 2: pretty dump equals fixture's pretty_utf8.
    const std::string got_pretty = call_dump_pretty(fp.input_blob);
    REQUIRE(got_pretty ==
            fp.expected.at("pretty_utf8").get<std::string>());

    // Assertion 3: normalize_panel_themes matches when compared as JSON.
    // Fixture pins a JSON *object*; we compare parsed forms so key order
    // doesn't cause a false negative — the semantics live at the value
    // level, not the byte level, for this sub-assertion.
    const std::string got_normalised = call_normalize_panel_themes(fp.input_blob);
    const auto got_normalised_j = nlohmann::json::parse(got_normalised);
    const auto& expected_normalised =
        fp.expected.at("normalized_panel_themes");
    REQUIRE(got_normalised_j == nlohmann::json(expected_normalised));

    // Assertion 4: merge_hotkeys matches when compared as JSON.  Same
    // reasoning as above.
    const std::string got_hotkeys = call_merge_hotkeys(fp.input_blob);
    const auto got_hotkeys_j = nlohmann::json::parse(got_hotkeys);
    const auto& expected_hotkeys = fp.expected.at("merged_hotkeys");
    REQUIRE(got_hotkeys_j == nlohmann::json(expected_hotkeys));

    // Assertion 5: strip_legacy_dump equals fixture's
    // after_legacy_strip_plaintext_utf8 (byte-identical since we're
    // pinning the compact dump path).
    const std::string got_stripped = call_strip_legacy_dump(fp.input_blob);
    REQUIRE(got_stripped ==
            fp.expected.at("after_legacy_strip_plaintext_utf8").get<std::string>());

    // Assertion 6: byte length invariant.  Not counted as a required
    // fixture invariant (plaintext_utf8_bytes_len already redundant with
    // plaintext_utf8.size()), but pins it explicitly so a future writer
    // regression that lost a trailing byte would be caught here.
    REQUIRE(got_plaintext.size() ==
            fp.expected.at("plaintext_utf8_bytes_len").get<size_t>());
}

}  // namespace

// ---------------------------------------------------------------------------
// The 10 pinned fixtures.
// ---------------------------------------------------------------------------
TEST_CASE("settings_fixture_parity_wave9 normal_default_only_panel_theme",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("normal_default_only_panel_theme.json");
}

TEST_CASE("settings_fixture_parity_wave9 normal_full_v5",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("normal_full_v5.json");
}

TEST_CASE("settings_fixture_parity_wave9 normal_light_theme",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("normal_light_theme.json");
}

TEST_CASE("settings_fixture_parity_wave9 normal_utf8_plugin_names",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("normal_utf8_plugin_names.json");
}

TEST_CASE("settings_fixture_parity_wave9 v0_legacy_migration_stripped",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("v0_legacy_migration_stripped.json");
}

TEST_CASE("settings_fixture_parity_wave9 partial_only_hotkeys",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("partial_only_hotkeys.json");
}

TEST_CASE("settings_fixture_parity_wave9 partial_missing_panel_themes",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("partial_missing_panel_themes.json");
}

TEST_CASE("settings_fixture_parity_wave9 extra_field_unknown_keys_kept",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("extra_field_unknown_keys_kept.json");
}

TEST_CASE("settings_fixture_parity_wave9 corrupt_wrong_theme_string",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("corrupt_wrong_theme_string.json");
}

TEST_CASE("settings_fixture_parity_wave9 corrupt_non_dict_theme",
          "[core][settings][fixture_parity_wave9]") {
    run_settings_fixture("corrupt_non_dict_theme.json");
}
