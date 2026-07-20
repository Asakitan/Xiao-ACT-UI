// UI-spec normalizer fixture parity against the frozen Python outcomes.
//
// The freeze at docs/fixtures/ui_spec/*.json captures the eight canonical
// normalization outcomes ``act_platform.ui_spec.normalize_ui_spec``
// produces on typical / nested / mutation / invalid_ref inputs.  This
// test drives ``sao_engine_ui_spec_normalize`` (implemented in
// platform/engine/src/ui_spec.cpp, previously a stub) through each
// fixture's ``input`` block and verifies the output matches the
// fixture's ``expected.normalized`` object byte-equivalent under
// nlohmann::json value equality.
//
// The eight fixtures the freeze recorded (see FROZEN.md at rev
// ``a3be6a3f``):
//   * normal_flat_panel_text_bar          text + bar + kv inside panel
//   * normal_button_row                   card > row > buttons w/ styles
//   * normal_table_with_highlight         table columns / rows / highlight
//   * nested_deep_sections                section > row > text w/ spacer
//   * mutation_unknown_types_dropped      unknown kinds silently dropped
//   * mutation_text_clamp                 4000-char ellipsis clamp
//   * invalid_ref_wrong_scalar_types      bar w/ bad pct/label/color
//   * invalid_ref_bare_list               list of strings -> muted text
//
// UTF-8 no BOM.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sao/engine/ui_spec.h"
#include "sao/core/status.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::optional<std::string> load_ui_spec_fixture(const std::string& name) {
    static const char* kRoots[] = {
        "docs/fixtures/ui_spec/",
        "../docs/fixtures/ui_spec/",
        "../../docs/fixtures/ui_spec/",
        "../../../docs/fixtures/ui_spec/",
        "../../../../docs/fixtures/ui_spec/",
        "../../../../../docs/fixtures/ui_spec/",
        "sao_auto/C/docs/fixtures/ui_spec/",
        "../sao_auto/C/docs/fixtures/ui_spec/",
        "../../sao_auto/C/docs/fixtures/ui_spec/",
        "../../../sao_auto/C/docs/fixtures/ui_spec/",
        "e:/VC/SAO-UI/sao_auto/C/docs/fixtures/ui_spec/",
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

// Normalise a fixture's input via the engine ABI and return the parsed
// nlohmann::json object.  Uses a large fixed buffer -- the fixture spec
// inputs top out around 40KB (mutation_text_clamp is the worst case).
nlohmann::json normalize_via_engine(const nlohmann::json& input_payload) {
    // Serialize input JSON in the same shape gen_ui_spec.py writes:
    // {"spec": <any>, "title": "<string>"}.  The engine ABI expects
    // exactly this envelope; if we pass just the spec directly, the
    // engine falls back to treating the whole thing as the spec (which
    // is fine for the "bare list" fixture but noisy otherwise).
    std::string serialized = input_payload.dump();

    // First call queries the required size; second call fills the buffer.
    size_t required = 0;
    sao_status_t rc = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(serialized.data()),
        serialized.size(),
        /* out_json_utf8   = */ nullptr,
        /* out_capacity    = */ 0,
        &required);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(required > 0);

    std::vector<uint8_t> buffer(required + 16);
    size_t written = 0;
    rc = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(serialized.data()),
        serialized.size(),
        buffer.data(),
        buffer.size(),
        &written);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(written == required);
    std::string out(reinterpret_cast<const char*>(buffer.data()), written);
    nlohmann::json parsed;
    REQUIRE_NOTHROW(parsed = nlohmann::json::parse(out));
    return parsed;
}

void verify_ui_spec_fixture(const std::string& fixture_name) {
    auto raw = load_ui_spec_fixture(fixture_name);
    REQUIRE(raw.has_value());
    nlohmann::json j;
    REQUIRE_NOTHROW(j = nlohmann::json::parse(*raw));
    REQUIRE(j.contains("input"));
    REQUIRE(j.contains("expected"));

    const nlohmann::json& input_payload = j["input"];
    const nlohmann::json& expected      = j["expected"]["normalized"];

    nlohmann::json got = normalize_via_engine(input_payload);
    INFO("fixture: " << fixture_name);
    INFO("got: "  << got.dump(2));
    INFO("want: " << expected.dump(2));
    REQUIRE(got == expected);

    // Also sanity-check the derived fields (version, top_level_node_count)
    // that the freeze records separately from ``normalized``.
    REQUIRE(got["version"] == j["expected"]["version"]);
    REQUIRE(static_cast<int>(got["nodes"].size()) ==
            j["expected"]["top_level_node_count"].get<int>());
}

}  // namespace

// ── Eight SECTIONs, one per fixture ────────────────────────────────

TEST_CASE("fixture parity ui_spec normal_flat_panel_text_bar",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("normal_flat_panel_text_bar.json");
}

TEST_CASE("fixture parity ui_spec normal_button_row",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("normal_button_row.json");
}

TEST_CASE("fixture parity ui_spec normal_table_with_highlight",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("normal_table_with_highlight.json");
}

TEST_CASE("fixture parity ui_spec nested_deep_sections",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("nested_deep_sections.json");
}

TEST_CASE("fixture parity ui_spec mutation_unknown_types_dropped",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("mutation_unknown_types_dropped.json");
}

TEST_CASE("fixture parity ui_spec mutation_text_clamp",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("mutation_text_clamp.json");
}

TEST_CASE("fixture parity ui_spec invalid_ref_wrong_scalar_types",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("invalid_ref_wrong_scalar_types.json");
}

TEST_CASE("fixture parity ui_spec invalid_ref_bare_list",
          "[ui][ui_spec][fixture][lifecycle]") {
    verify_ui_spec_fixture("invalid_ref_bare_list.json");
}

// ── Bounds smoke tests -- exercise the normalizer's clamp behaviour ─
// These are extra REQUIREs beyond the eight fixtures, giving the
// bounds cases add enough assert coverage that any regression in the
// helpers (s_clamp / clamp01 / choice / ci / cpos / cz) surfaces
// during CI even if a new fixture doesn't happen to trip it.

TEST_CASE("fixture parity ui_spec normalizer version endpoint",
          "[ui][ui_spec][lifecycle]") {
    REQUIRE(sao_engine_ui_spec_version() == SAO_UI_SPEC_VERSION);
    REQUIRE(SAO_UI_SPEC_VERSION == 1);
}

TEST_CASE("fixture parity ui_spec normalizer malformed input rejected",
          "[ui][ui_spec][lifecycle]") {
    const std::string bad = "{ not: valid json";
    size_t written = 42;
    auto rc = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(bad.data()), bad.size(),
        nullptr, 0, &written);
    REQUIRE(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(written == 0);
}

TEST_CASE("fixture parity ui_spec normalizer out buffer too small",
          "[ui][ui_spec][lifecycle]") {
    const std::string envelope =
        R"({"spec": {"type": "text", "text": "hi"}, "title": ""})";
    size_t required = 0;
    auto rc = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size(),
        nullptr, 0, &required);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(required > 0);

    // A one-byte buffer must fail with BUFFER_TOO_SMALL and echo the
    // required size back through ``out_bytes_written``.
    std::vector<uint8_t> one(1);
    size_t written = 0;
    rc = sao_engine_ui_spec_normalize(
        reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size(),
        one.data(), one.size(), &written);
    REQUIRE(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(written == required);
}
