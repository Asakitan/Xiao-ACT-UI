// SAO Auto — platform/engine/tests/test_state_machine_wave6.cpp
//
// Wave 6 / Phase 5 — generic state machine coverage.
//
// Six scenarios exercise the game-agnostic contract:
//   * create + get_current returns the initial state
//   * dispatch advances the state along a declared edge
//   * illegal event returns SAO_STATUS_ERR_INVALID_TRANSITION w/o mutation
//   * history captures every accepted transition in order
//   * reset returns to initial and clears history
//   * capacity-preserving buffer_used probe (out_entries == null)
//
// The config JSON blobs use abstract state names ("A", "B", ...) —
// the engine layer must not require any game-specific vocabulary.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "sao/engine/state.h"

namespace {

constexpr const char* kMinimalConfig = R"({
    "initial": "A",
    "states": ["A", "B", "C"],
    "transitions": [
        {"from": "A", "to": "B", "on": ["go"]},
        {"from": "B", "to": "C", "on": ["next", "advance"]},
        {"from": "C", "to": "A", "on": ["reset"]}
    ]
})";

std::string readCurrent(sao_engine_state_machine_handle_t handle) {
    char buf[64] = {0};
    auto rc = sao_engine_state_machine_get_current(handle, buf, sizeof(buf));
    REQUIRE(rc == SAO_STATUS_OK);
    return std::string(buf);
}

}  // namespace

TEST_CASE("state_machine_create_returns_initial_state",
          "[engine][state_machine][wave6]") {
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);
    REQUIRE(sm != nullptr);
    REQUIRE(readCurrent(sm) == "A");
    sao_engine_state_machine_destroy(sm);
}

TEST_CASE("state_machine_dispatch_advances_along_declared_edge",
          "[engine][state_machine][wave6]") {
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);

    char state_buf[64] = {0};
    REQUIRE(sao_engine_state_machine_dispatch(sm, "go", 1000, state_buf, sizeof(state_buf))
            == SAO_STATUS_OK);
    REQUIRE(std::string(state_buf) == "B");
    REQUIRE(readCurrent(sm) == "B");

    // Second edge with alternative event tokens.
    REQUIRE(sao_engine_state_machine_dispatch(sm, "advance", 2000, state_buf, sizeof(state_buf))
            == SAO_STATUS_OK);
    REQUIRE(std::string(state_buf) == "C");

    sao_engine_state_machine_destroy(sm);
}

TEST_CASE("state_machine_illegal_event_returns_invalid_transition",
          "[engine][state_machine][wave6]") {
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);

    char state_buf[64] = {0};
    auto rc = sao_engine_state_machine_dispatch(sm, "not_a_declared_event",
                                                 100, state_buf, sizeof(state_buf));
    REQUIRE(rc == SAO_STATUS_ERR_INVALID_TRANSITION);
    // Current state was NOT mutated — buffer still holds it.
    REQUIRE(std::string(state_buf) == "A");
    REQUIRE(readCurrent(sm) == "A");

    sao_engine_state_machine_destroy(sm);
}

TEST_CASE("state_machine_history_records_accepted_transitions",
          "[engine][state_machine][wave6]") {
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);

    // Only accepted transitions must land in history.
    REQUIRE(sao_engine_state_machine_dispatch(sm, "go", 100, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_engine_state_machine_dispatch(sm, "denied", 150, nullptr, 0)
            == SAO_STATUS_ERR_INVALID_TRANSITION);
    REQUIRE(sao_engine_state_machine_dispatch(sm, "next", 200, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_engine_state_machine_dispatch(sm, "reset", 300, nullptr, 0) == SAO_STATUS_OK);

    // Probe entry count first.
    uint32_t count = 0;
    size_t used = 0;
    REQUIRE(sao_engine_state_machine_get_history(sm, 0, nullptr, 0, nullptr, 0,
                                                  &count, &used) == SAO_STATUS_OK);
    REQUIRE(count == 3);
    REQUIRE(used > 0);

    std::vector<SaoEngineStateTransition> entries(count);
    std::vector<char> buffer(used);
    REQUIRE(sao_engine_state_machine_get_history(sm, count, entries.data(), entries.size(),
                                                  buffer.data(), buffer.size(),
                                                  &count, &used) == SAO_STATUS_OK);
    REQUIRE(count == 3);

    auto slice = [&](uint32_t off, uint32_t len) {
        return std::string(buffer.data() + off, len);
    };
    REQUIRE(slice(entries[0].from_offset, entries[0].from_length) == "A");
    REQUIRE(slice(entries[0].to_offset,   entries[0].to_length)   == "B");
    REQUIRE(slice(entries[0].event_offset,entries[0].event_length)== "go");
    REQUIRE(entries[0].timestamp_ns == 100);

    REQUIRE(slice(entries[1].from_offset, entries[1].from_length) == "B");
    REQUIRE(slice(entries[1].to_offset,   entries[1].to_length)   == "C");
    REQUIRE(slice(entries[1].event_offset,entries[1].event_length)== "next");
    REQUIRE(entries[1].timestamp_ns == 200);

    REQUIRE(slice(entries[2].from_offset, entries[2].from_length) == "C");
    REQUIRE(slice(entries[2].to_offset,   entries[2].to_length)   == "A");
    REQUIRE(slice(entries[2].event_offset,entries[2].event_length)== "reset");
    REQUIRE(entries[2].timestamp_ns == 300);

    sao_engine_state_machine_destroy(sm);
}

TEST_CASE("state_machine_reset_clears_state_and_history",
          "[engine][state_machine][wave6]") {
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);

    REQUIRE(sao_engine_state_machine_dispatch(sm, "go", 100, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_engine_state_machine_dispatch(sm, "next", 200, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(readCurrent(sm) == "C");

    REQUIRE(sao_engine_state_machine_reset(sm) == SAO_STATUS_OK);
    REQUIRE(readCurrent(sm) == "A");

    uint32_t count = 99;
    size_t used = 99;
    REQUIRE(sao_engine_state_machine_get_history(sm, 0, nullptr, 0, nullptr, 0,
                                                  &count, &used) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    REQUIRE(used == 0);

    sao_engine_state_machine_destroy(sm);
}

TEST_CASE("state_machine_history_query_reports_capacity",
          "[engine][state_machine][wave6]") {
    // Buffer too small propagates but the count / bytes still report
    // what the caller would need.  This is what enables the
    // two-call size-then-fetch idiom used by both Tk and WebView paths.
    sao_engine_state_machine_handle_t sm = nullptr;
    REQUIRE(sao_engine_state_machine_create(kMinimalConfig, &sm) == SAO_STATUS_OK);

    REQUIRE(sao_engine_state_machine_dispatch(sm, "go", 100, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_engine_state_machine_dispatch(sm, "next", 200, nullptr, 0) == SAO_STATUS_OK);

    uint32_t count = 0;
    size_t used = 0;
    REQUIRE(sao_engine_state_machine_get_history(sm, 0, nullptr, 0, nullptr, 0,
                                                  &count, &used) == SAO_STATUS_OK);
    REQUIRE(count == 2);
    REQUIRE(used > 0);

    // Buffer too small: entries capacity is 1, but two transitions exist.
    SaoEngineStateTransition entries[1] = {};
    std::vector<char> buffer(used);
    auto rc = sao_engine_state_machine_get_history(sm, 0, entries, 1,
                                                    buffer.data(), buffer.size(),
                                                    &count, &used);
    REQUIRE(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    sao_engine_state_machine_destroy(sm);
}
