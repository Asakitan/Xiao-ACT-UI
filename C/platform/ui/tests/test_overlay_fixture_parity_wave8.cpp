// Wave 8 / Agent c - overlay compositor fixture parity, Phase 6 deep dive.
//
// The freeze at docs/fixtures/overlay/*.json captures the pure state
// transitions Python's OverlayCompositor performs on layer add / show /
// hide / raise / destroy / tick events.  The full OverlayCompositor class
// itself requires a live GL + DirectComposition context so it cannot be
// instantiated headless -- what CAN be canonicalised are the transitions
// exercised by ``tools/gen_fixtures/gen_overlay.py::CompositorState``.
// That pure Python state-machine is the oracle; this file mirrors it 1:1
// in C++ and verifies the six fixture scenarios byte-for-byte against the
// snapshot payload the generator emits.
//
// This test does NOT touch sao::ui::compositor.cpp -- that module owns the
// live GL layer bookkeeping and its API is not shaped to mirror Python
// CompositorState's "layers survive destroy_layer as epoch-only records"
// semantics.  A future compositor.cpp revamp can hook in via a shared
// helper, but the parity gate lives here so drift is diagnosed against
// the freeze byte stream, not against internal C++ types.
//
// UTF-8 no BOM.  Six Catch2 SECTIONs cover:
//   * normal_add_show_reorder                    - basic z/reorder/visibility
//   * topmost_race_deterministic                 - repeated raise_layer converges
//   * gdi_leak_scenario_destroy_and_recreate     - name reuse bumps epoch
//   * focus_shield_edge_click_through_false      - input proxy attaches once
//   * present_tick_normal_dirty_flag             - tick + set_pos dirty logic
//   * z_order_tie_by_insertion                   - equal z ties break on
//                                                  creation order

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// ── Filesystem search for docs/fixtures/overlay/*.json ──────────────
// CTest working directory depends on the preset; walk a few plausible
// starting points.  Every entry rooted at sao_auto/C.
std::optional<std::string> load_overlay_fixture(const std::string& name) {
    static const char* kRoots[] = {
        "docs/fixtures/overlay/",
        "../docs/fixtures/overlay/",
        "../../docs/fixtures/overlay/",
        "../../../docs/fixtures/overlay/",
        "../../../../docs/fixtures/overlay/",
        "../../../../../docs/fixtures/overlay/",
        "sao_auto/C/docs/fixtures/overlay/",
        "../sao_auto/C/docs/fixtures/overlay/",
        "../../sao_auto/C/docs/fixtures/overlay/",
        "../../../sao_auto/C/docs/fixtures/overlay/",
        "e:/VC/SAO-UI/sao_auto/C/docs/fixtures/overlay/",
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

// ── Python CompositorState 1:1 port ────────────────────────────────
//
// Mirrors ``tools/gen_fixtures/gen_overlay.py::CompositorState`` node
// for node.  Field order + serialisation order does NOT matter here
// because we round-trip through nlohmann::json (default alphabetical
// key ordering) so the produced JSON matches Python's sort_keys=True
// naturally.
struct LayerState {
    std::string name;
    int32_t     x{0};
    int32_t     y{0};
    int32_t     width{1};
    int32_t     height{1};
    int32_t     z_order{0};
    bool        visible{false};
    bool        click_through{true};
    int32_t     epoch{1};
    bool        input_proxy_attached{false};
    int32_t     insertion_order{0};
};

struct CompositorState {
    // Preserve insertion order for JSON keys inside "layers" / "epochs"
    // even though nlohmann::json default sort_keys covers it.  Using
    // std::vector + lookup avoids depending on ordered_json.
    std::unordered_map<std::string, LayerState> layers;
    std::unordered_map<std::string, int32_t>    epochs;
    bool                                        dirty{false};
    int32_t                                     insertion_counter{0};
    std::vector<std::string>                    z_sorted_names;
    // Union of visible layer bounding rects.  Only rebuilt on
    // create/destroy/set_layer_z/raise -- matches Python quirk exactly.
    std::vector<std::array<int32_t, 4>>         host_region_rects;
    int32_t                                     layers_changed_count{0};
    int32_t                                     layer_retirements{0};
    int32_t                                     input_proxy_attachments{0};

    void rebuild_z_order() {
        // Sort by (z_order asc, insertion_order asc) -- ties broken by
        // insertion sequence, so the layer created first draws first.
        std::vector<const LayerState*> sorted;
        sorted.reserve(layers.size());
        for (const auto& kv : layers) sorted.push_back(&kv.second);
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const LayerState* a, const LayerState* b) {
                             if (a->z_order != b->z_order) {
                                 return a->z_order < b->z_order;
                             }
                             return a->insertion_order < b->insertion_order;
                         });
        z_sorted_names.clear();
        z_sorted_names.reserve(sorted.size());
        host_region_rects.clear();
        for (const LayerState* l : sorted) {
            z_sorted_names.push_back(l->name);
            if (l->visible) {
                host_region_rects.push_back({
                    l->x, l->y, l->x + l->width, l->y + l->height,
                });
            }
        }
    }

    void create_layer(const std::string& name, int w, int h, int x, int y,
                      int z, bool click_through) {
        auto it = layers.find(name);
        int prev_epoch = 0;
        auto ep = epochs.find(name);
        if (ep != epochs.end()) prev_epoch = ep->second;
        int new_epoch = prev_epoch + 1;
        epochs[name] = new_epoch;
        if (it != layers.end()) {
            layer_retirements += 1;
            dirty = true;
        }
        insertion_counter += 1;
        LayerState layer;
        layer.name             = name;
        layer.x                = x;
        layer.y                = y;
        layer.width            = std::max(1, w);
        layer.height           = std::max(1, h);
        layer.z_order          = z;
        layer.visible          = false;
        layer.click_through    = click_through;
        layer.epoch            = new_epoch;
        layer.insertion_order  = insertion_counter;
        if (!click_through) {
            layer.input_proxy_attached = true;
            input_proxy_attachments += 1;
        }
        layers[name] = layer;
        rebuild_z_order();
    }

    void destroy_layer(const std::string& name) {
        auto it = layers.find(name);
        // Bump epoch regardless (Python does epochs[name] = epochs.get + 1)
        auto ep = epochs.find(name);
        int prev_epoch = (ep == epochs.end()) ? 0 : ep->second;
        epochs[name] = prev_epoch + 1;
        if (it != layers.end()) {
            LayerState prev = it->second;
            layer_retirements += 1;
            if (prev.visible) {
                dirty = true;
                layers_changed_count += 1;
            }
            layers.erase(it);
        }
        rebuild_z_order();
    }

    void set_visible(const std::string& name, bool visible) {
        auto it = layers.find(name);
        if (it == layers.end()) return;
        if (visible != it->second.visible) {
            it->second.visible = visible;
            dirty = true;
            layers_changed_count += 1;
        }
    }

    void set_layer_z(const std::string& name, int z) {
        auto it = layers.find(name);
        if (it == layers.end()) return;
        it->second.z_order = z;
        rebuild_z_order();
    }

    void raise_layer(const std::string& name) {
        if (layers.empty()) return;
        int max_z = layers.begin()->second.z_order;
        for (const auto& kv : layers) {
            if (kv.second.z_order > max_z) max_z = kv.second.z_order;
        }
        auto it = layers.find(name);
        if (it == layers.end()) return;
        it->second.z_order = max_z + 1;
        rebuild_z_order();
    }

    void set_position(const std::string& name, int x, int y) {
        auto it = layers.find(name);
        if (it == layers.end()) return;
        it->second.x = x;
        it->second.y = y;
        if (it->second.visible) dirty = true;
    }

    // Return whether the compositor would present this tick.  Clears
    // dirty on present.
    bool apply_tick(bool has_frame_updates) {
        bool present = has_frame_updates || dirty;
        if (present) dirty = false;
        return present;
    }
};

// ── Snapshot serializer ────────────────────────────────────────────
// Emits nlohmann::json objects matching the shape the Python generator
// writes.  Field order is irrelevant here because nlohmann::json sorts
// alphabetically at dump time; we just have to output equivalent VALUES.

nlohmann::json layer_snapshot(const LayerState& l) {
    // Order in the JSON output is dictated by nlohmann::json's ordered
    // map (which for standard `nlohmann::json` is `std::map` -- sorted).
    nlohmann::json j;
    j["click_through"]        = l.click_through;
    j["epoch"]                = l.epoch;
    j["height"]               = l.height;
    j["input_proxy_attached"] = l.input_proxy_attached;
    j["insertion_order"]      = l.insertion_order;
    j["name"]                 = l.name;
    j["visible"]              = l.visible;
    j["width"]                = l.width;
    j["x"]                    = l.x;
    j["y"]                    = l.y;
    j["z_order"]              = l.z_order;
    return j;
}

nlohmann::json compositor_snapshot(const CompositorState& s) {
    nlohmann::json j;
    j["dirty"]                    = s.dirty;
    nlohmann::json epochs_j = nlohmann::json::object();
    for (const auto& kv : s.epochs) epochs_j[kv.first] = kv.second;
    j["epochs"]                   = epochs_j;
    nlohmann::json rects_j = nlohmann::json::array();
    for (const auto& r : s.host_region_rects) {
        rects_j.push_back({r[0], r[1], r[2], r[3]});
    }
    j["host_region_rects"]        = rects_j;
    j["input_proxy_attachments"]  = s.input_proxy_attachments;
    j["insertion_counter"]        = s.insertion_counter;
    j["layer_retirements"]        = s.layer_retirements;
    nlohmann::json layers_j = nlohmann::json::object();
    for (const auto& kv : s.layers) layers_j[kv.first] = layer_snapshot(kv.second);
    j["layers"]                   = layers_j;
    j["layers_changed_count"]     = s.layers_changed_count;
    nlohmann::json z_j = nlohmann::json::array();
    for (const auto& n : s.z_sorted_names) z_j.push_back(n);
    j["z_sorted_names"]           = z_j;
    return j;
}

// ── Event runner ──────────────────────────────────────────────────
struct StepLogEntry {
    nlohmann::json data;
};

std::pair<nlohmann::json, std::vector<nlohmann::json>>
run_scenario(const nlohmann::json& events) {
    CompositorState state;
    std::vector<nlohmann::json> steps;
    for (const auto& evt : events) {
        REQUIRE(evt.is_array());
        REQUIRE(evt.size() >= 1);
        const std::string op = evt[0].get<std::string>();

        if (op == "create") {
            REQUIRE(evt.size() == 8);
            const std::string name = evt[1].get<std::string>();
            const int w = evt[2].get<int>();
            const int h = evt[3].get<int>();
            const int x = evt[4].get<int>();
            const int y = evt[5].get<int>();
            const int z = evt[6].get<int>();
            const bool ct = evt[7].get<bool>();
            state.create_layer(name, w, h, x, y, z, ct);
        } else if (op == "destroy") {
            REQUIRE(evt.size() == 2);
            state.destroy_layer(evt[1].get<std::string>());
        } else if (op == "show") {
            REQUIRE(evt.size() == 3);
            state.set_visible(evt[1].get<std::string>(),
                              evt[2].get<bool>());
        } else if (op == "raise") {
            REQUIRE(evt.size() == 2);
            state.raise_layer(evt[1].get<std::string>());
        } else if (op == "set_pos") {
            REQUIRE(evt.size() == 4);
            state.set_position(evt[1].get<std::string>(),
                               evt[2].get<int>(),
                               evt[3].get<int>());
        } else if (op == "set_z") {
            REQUIRE(evt.size() == 3);
            state.set_layer_z(evt[1].get<std::string>(),
                              evt[2].get<int>());
        } else if (op == "tick") {
            REQUIRE(evt.size() == 2);
            bool has_frame = evt[1].get<bool>();
            bool presented = state.apply_tick(has_frame);
            nlohmann::json step_entry;
            step_entry["has_frame"] = has_frame;
            step_entry["op"]        = "tick";
            step_entry["presented"] = presented;
            steps.push_back(step_entry);
            continue;
        } else {
            FAIL("unknown event op: " + op);
        }

        nlohmann::json step_entry;
        step_entry["dirty"] = state.dirty;
        nlohmann::json event_tail = nlohmann::json::array();
        for (size_t i = 1; i < evt.size(); ++i) event_tail.push_back(evt[i]);
        step_entry["event"] = event_tail;
        step_entry["op"]    = op;
        nlohmann::json z_arr = nlohmann::json::array();
        for (const auto& n : state.z_sorted_names) z_arr.push_back(n);
        step_entry["z_sorted_names"] = z_arr;
        steps.push_back(step_entry);
    }
    return {compositor_snapshot(state), steps};
}

// Verify a single fixture end-to-end.  Load the JSON, feed events into
// the C++ state machine, compare final_state + steps object-equal to
// the fixture's expected block.  nlohmann::json's operator== does a
// value comparison so key insertion order does not matter.
void verify_overlay_fixture(const std::string& fixture_name) {
    auto raw = load_overlay_fixture(fixture_name);
    REQUIRE(raw.has_value());
    nlohmann::json j;
    REQUIRE_NOTHROW(j = nlohmann::json::parse(*raw));
    REQUIRE(j.contains("input"));
    REQUIRE(j.contains("expected"));
    REQUIRE(j["input"].contains("events"));
    REQUIRE(j["expected"].contains("final_state"));
    REQUIRE(j["expected"].contains("steps"));

    auto [final_state, step_log] = run_scenario(j["input"]["events"]);

    const nlohmann::json& expected_final = j["expected"]["final_state"];
    const nlohmann::json& expected_steps = j["expected"]["steps"];

    // Byte-diff via string dump (sort_keys mode).  If the comparison
    // fails we get a readable diff instead of catch2's opaque
    // "REQUIRE(a == b)" report.
    const std::string got_final     = final_state.dump(2);
    const std::string want_final    = expected_final.dump(2);
    const std::string got_steps     = nlohmann::json(step_log).dump(2);
    const std::string want_steps    = expected_steps.dump(2);

    INFO("fixture: " << fixture_name);
    INFO("final_state diff (got vs want):\n" << got_final << "\nvs\n" << want_final);
    REQUIRE(final_state == expected_final);
    INFO("steps diff (got vs want):\n" << got_steps << "\nvs\n" << want_steps);
    REQUIRE(nlohmann::json(step_log) == expected_steps);
}

}  // namespace

// ── Six SECTIONs, one per fixture ─────────────────────────────────

TEST_CASE("fixture_parity_wave8 overlay -normal_add_show_reorder",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("normal_add_show_reorder.json");
}

TEST_CASE("fixture_parity_wave8 overlay -topmost_race_deterministic",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("topmost_race_deterministic.json");
}

TEST_CASE("fixture_parity_wave8 overlay -gdi_leak_scenario_destroy_and_recreate",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("gdi_leak_scenario_destroy_and_recreate.json");
}

TEST_CASE("fixture_parity_wave8 overlay -focus_shield_edge_click_through_false",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("focus_shield_edge_click_through_false.json");
}

TEST_CASE("fixture_parity_wave8 overlay -present_tick_normal_dirty_flag",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("present_tick_normal_dirty_flag.json");
}

TEST_CASE("fixture_parity_wave8 overlay -z_order_tie_by_insertion",
          "[ui][overlay][fixture][wave8]") {
    verify_overlay_fixture("z_order_tie_by_insertion.json");
}
