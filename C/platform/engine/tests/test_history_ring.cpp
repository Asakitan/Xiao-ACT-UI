// SAO Auto — platform/engine/tests/test_history_ring.cpp
//
// Wave 6 / Phase 5 — generic history ring buffer coverage.
//
// Six scenarios exercise the game-agnostic contract:
//   * push + get returns the newest entry at index 0
//   * overwriting past capacity keeps only the last N entries
//   * negative indexing (Python-style) maps to oldest-first
//   * range() returns chronological entries inside [start_ms, end_ms]
//   * clear() empties the ring but preserves capacity/entry_size
//   * buffer-too-small on range() still reports total match count
//
// Entries are opaque byte blobs.  The tests use plain integer
// payloads to keep the fixtures readable, but the ring itself has
// no knowledge of the payload shape.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "sao/engine/history_ring.h"

namespace {

struct Payload {
    int32_t value;
    int32_t tag;
};

Payload readAt(sao_engine_history_ring_handle_t ring, int32_t index) {
    Payload out{};
    uint64_t ts = 0;
    auto rc = sao_engine_history_ring_get(ring, index, &out, &ts);
    REQUIRE(rc == SAO_STATUS_OK);
    return out;
}

}  // namespace

TEST_CASE("history_ring_push_get_newest_at_index_zero",
          "[engine][history_ring][automation]") {
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(4, sizeof(Payload), &ring) == SAO_STATUS_OK);
    REQUIRE(ring != nullptr);

    Payload a{1, 100};
    Payload b{2, 200};
    Payload c{3, 300};
    REQUIRE(sao_engine_history_ring_push(ring, &a, 1000) == SAO_STATUS_OK);
    REQUIRE(sao_engine_history_ring_push(ring, &b, 2000) == SAO_STATUS_OK);
    REQUIRE(sao_engine_history_ring_push(ring, &c, 3000) == SAO_STATUS_OK);

    auto newest = readAt(ring, 0);
    REQUIRE(newest.value == 3);
    REQUIRE(newest.tag   == 300);

    // Index into the middle.
    auto mid = readAt(ring, 1);
    REQUIRE(mid.value == 2);

    // Oldest via count-1.
    auto oldest = readAt(ring, 2);
    REQUIRE(oldest.value == 1);

    // Timestamps also flow through.
    Payload out{};
    uint64_t ts = 0;
    REQUIRE(sao_engine_history_ring_get(ring, 0, &out, &ts) == SAO_STATUS_OK);
    REQUIRE(ts == 3000);

    sao_engine_history_ring_destroy(ring);
}

TEST_CASE("history_ring_overwrites_oldest_past_capacity",
          "[engine][history_ring][automation]") {
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(3, sizeof(Payload), &ring) == SAO_STATUS_OK);

    for (int i = 1; i <= 5; ++i) {
        Payload p{i, i * 10};
        REQUIRE(sao_engine_history_ring_push(ring, &p, static_cast<uint64_t>(i) * 1000)
                == SAO_STATUS_OK);
    }
    // Only the last three (3, 4, 5) survive.
    REQUIRE(readAt(ring, 0).value == 5);
    REQUIRE(readAt(ring, 1).value == 4);
    REQUIRE(readAt(ring, 2).value == 3);

    sao_engine_history_ring_destroy(ring);
}

TEST_CASE("history_ring_negative_indexing_python_style",
          "[engine][history_ring][automation]") {
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(5, sizeof(Payload), &ring) == SAO_STATUS_OK);

    for (int i = 1; i <= 4; ++i) {
        Payload p{i, 0};
        REQUIRE(sao_engine_history_ring_push(ring, &p, static_cast<uint64_t>(i) * 100)
                == SAO_STATUS_OK);
    }
    // -1 = oldest = first pushed.
    REQUIRE(readAt(ring, -1).value == 1);
    REQUIRE(readAt(ring, -2).value == 2);
    // -count = newest.
    REQUIRE(readAt(ring, -4).value == 4);

    // Out-of-range indices return NOT_FOUND.
    Payload out{};
    REQUIRE(sao_engine_history_ring_get(ring, 99, &out, nullptr) == SAO_STATUS_ERR_NOT_FOUND);
    REQUIRE(sao_engine_history_ring_get(ring, -99, &out, nullptr) == SAO_STATUS_ERR_NOT_FOUND);

    sao_engine_history_ring_destroy(ring);
}

TEST_CASE("history_ring_range_returns_chronological_slice",
          "[engine][history_ring][automation]") {
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(8, sizeof(Payload), &ring) == SAO_STATUS_OK);

    // Timestamps at 100, 200, ..., 800.
    for (int i = 1; i <= 8; ++i) {
        Payload p{i, 0};
        REQUIRE(sao_engine_history_ring_push(ring, &p, static_cast<uint64_t>(i) * 100)
                == SAO_STATUS_OK);
    }
    // Grab entries in [250, 650].
    std::vector<Payload> buf(8);
    std::vector<uint64_t> ts_buf(8);
    uint32_t count = 0;
    REQUIRE(sao_engine_history_ring_range(ring, 250, 650, buf.data(), ts_buf.data(),
                                           static_cast<uint32_t>(buf.size()), &count)
            == SAO_STATUS_OK);
    // 300, 400, 500, 600 -> values 3..6, chronological order.
    REQUIRE(count == 4);
    REQUIRE(buf[0].value == 3);
    REQUIRE(buf[1].value == 4);
    REQUIRE(buf[2].value == 5);
    REQUIRE(buf[3].value == 6);
    REQUIRE(ts_buf[0] == 300);
    REQUIRE(ts_buf[3] == 600);

    sao_engine_history_ring_destroy(ring);
}

TEST_CASE("history_ring_clear_empties_ring",
          "[engine][history_ring][automation]") {
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(4, sizeof(Payload), &ring) == SAO_STATUS_OK);

    Payload p{42, 42};
    REQUIRE(sao_engine_history_ring_push(ring, &p, 1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_history_ring_push(ring, &p, 2) == SAO_STATUS_OK);

    REQUIRE(sao_engine_history_ring_clear(ring) == SAO_STATUS_OK);

    Payload out{};
    REQUIRE(sao_engine_history_ring_get(ring, 0, &out, nullptr) == SAO_STATUS_ERR_NOT_FOUND);

    // Ring must be usable again after clear (capacity/entry_size retained).
    REQUIRE(sao_engine_history_ring_push(ring, &p, 3) == SAO_STATUS_OK);
    REQUIRE(readAt(ring, 0).value == 42);

    sao_engine_history_ring_destroy(ring);
}

TEST_CASE("history_ring_range_buffer_too_small_reports_full_count",
          "[engine][history_ring][automation]") {
    // Callers can size a buffer via a first query-only pass.  When we
    // supply a payload buffer that's too small, ``count`` still tells
    // us how many entries *would* have been written.
    sao_engine_history_ring_handle_t ring = nullptr;
    REQUIRE(sao_engine_history_ring_create(5, sizeof(Payload), &ring) == SAO_STATUS_OK);

    for (int i = 1; i <= 5; ++i) {
        Payload p{i, 0};
        REQUIRE(sao_engine_history_ring_push(ring, &p, static_cast<uint64_t>(i) * 100)
                == SAO_STATUS_OK);
    }

    Payload buf[2] = {};
    uint32_t count = 0;
    auto rc = sao_engine_history_ring_range(ring, 0, 1000, buf, nullptr, 2, &count);
    REQUIRE(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(count == 5);
    // The first two slots did get written, in chronological order.
    REQUIRE(buf[0].value == 1);
    REQUIRE(buf[1].value == 2);

    sao_engine_history_ring_destroy(ring);
}
