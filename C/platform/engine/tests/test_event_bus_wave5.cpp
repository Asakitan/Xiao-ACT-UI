// SAO Auto — platform/engine/tests/test_event_bus_wave5.cpp
//
// Wave 5 / Phase 1 — event bus coverage.
//
// Six deterministic scenarios that pin the Python-equivalent semantics:
//   * subscribe / publish delivery
//   * priority ordering (three subscribers)
//   * cancellation stops propagation
//   * unsubscribe-during-fire behaves sanely
//   * publish to an empty topic is OK
//   * recursive publish (a subscriber publishes another event) works

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "sao/engine/event_bus.h"

namespace {

struct Bag {
    // Ordered record of "which subscriber fired" so tests can assert
    // the sequence.
    std::vector<std::string> seen;
};

int recordCallback(const char* topic, const uint8_t* /*data*/,
                   size_t /*data_size*/, void* user_data) {
    // The user_data pointer is a pair of (Bag*, tag).  For simplicity
    // we pack the tag string into a static-lifetime prefix stored in a
    // second parameter — but Catch2 tests use small helper structs
    // instead.  See ``TaggedBag`` below.
    (void)topic;
    (void)user_data;
    return SAO_ENGINE_EVENT_CONTINUE;
}

struct TaggedBag {
    Bag* bag;
    std::string tag;
    int return_code;
};

int taggedCallback(const char* /*topic*/, const uint8_t* /*data*/,
                   size_t /*data_size*/, void* user_data) {
    auto* tb = static_cast<TaggedBag*>(user_data);
    tb->bag->seen.push_back(tb->tag);
    return tb->return_code;
}

}  // namespace

TEST_CASE("event_bus_subscribe_publish_delivers_to_subscriber",
          "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag tb{&bag, "sub_a", SAO_ENGINE_EVENT_CONTINUE};
    sao_engine_subscription_t sub = 0;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "topic.x", 0,
                                                  &taggedCallback, &tb,
                                                  &sub) == SAO_STATUS_OK);
    REQUIRE(sub != 0);

    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "topic.x",
                                                nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 1);
    REQUIRE(bag.seen[0] == "sub_a");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_priority_ordering", "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag low  {&bag, "low",  SAO_ENGINE_EVENT_CONTINUE};
    TaggedBag mid  {&bag, "mid",  SAO_ENGINE_EVENT_CONTINUE};
    TaggedBag high {&bag, "high", SAO_ENGINE_EVENT_CONTINUE};

    // Register in the "wrong" order so the sort has real work to do.
    sao_engine_subscription_t s1 = 0, s2 = 0, s3 = 0;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 1,
                                                  &taggedCallback, &low,
                                                  &s1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 10,
                                                  &taggedCallback, &high,
                                                  &s2) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 5,
                                                  &taggedCallback, &mid,
                                                  &s3) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 3);
    REQUIRE(bag.seen[0] == "high");
    REQUIRE(bag.seen[1] == "mid");
    REQUIRE(bag.seen[2] == "low");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_cancel_stops_propagation",
          "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag first {&bag, "first",  SAO_ENGINE_EVENT_CONTINUE};
    // The second subscriber cancels; the third must never fire.
    TaggedBag second{&bag, "second", SAO_ENGINE_EVENT_CANCELLED};
    TaggedBag third {&bag, "third",  SAO_ENGINE_EVENT_CONTINUE};

    sao_engine_subscription_t s1 = 0, s2 = 0, s3 = 0;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 10,
                                                  &taggedCallback, &first,
                                                  &s1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 5,
                                                  &taggedCallback, &second,
                                                  &s2) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 1,
                                                  &taggedCallback, &third,
                                                  &s3) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 2);
    REQUIRE(bag.seen[0] == "first");
    REQUIRE(bag.seen[1] == "second");

    sao_engine_event_bus_destroy(bus);
}

// ---------------------------------------------------------------------------
// Mid-fire unsubscribe.
//
// Subscriber A, when fired, unsubscribes subscriber B.  We assert that:
//   1. A fires and runs to completion.
//   2. B either fires (if the snapshot was taken before A's callback ran)
//      or does not fire (if unsubscription took effect); either is
//      acceptable but the bus MUST NOT crash or double-invoke.
// ---------------------------------------------------------------------------
namespace {

struct UnsubStub {
    sao_engine_event_bus_handle_t bus = nullptr;
    sao_engine_subscription_t target = 0;
    Bag* bag = nullptr;
    bool ran = false;
};

int unsubberCallback(const char* /*topic*/, const uint8_t*, size_t,
                     void* user_data) {
    auto* u = static_cast<UnsubStub*>(user_data);
    u->ran = true;
    u->bag->seen.push_back("A_unsubs_B");
    (void)sao_engine_event_bus_unsubscribe(u->bus, u->target);
    return SAO_ENGINE_EVENT_CONTINUE;
}

}  // namespace

TEST_CASE("event_bus_unsubscribe_mid_fire",
          "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    Bag bag;
    UnsubStub a{};
    a.bus = bus;
    a.bag = &bag;

    TaggedBag b{&bag, "B_fired", SAO_ENGINE_EVENT_CONTINUE};

    // Subscribe B first so we know its token, then A which will remove
    // B mid-fire.  A has higher priority so it runs first.
    sao_engine_subscription_t bs = 0, as = 0;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 1,
                                                  &taggedCallback, &b,
                                                  &bs) == SAO_STATUS_OK);
    a.target = bs;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "t", 10,
                                                  &unsubberCallback, &a,
                                                  &as) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);

    REQUIRE(a.ran);
    // B may or may not have fired depending on snapshot-vs-live-check;
    // either behaviour is acceptable as long as bag.seen[0] is A and
    // there is no crash.
    REQUIRE(bag.seen.size() >= 1);
    REQUIRE(bag.seen[0] == "A_unsubs_B");
    // A second publish must see B gone.
    bag.seen.clear();
    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    // Only "A_unsubs_B" this time — B was pruned.
    REQUIRE(bag.seen.size() == 1);
    REQUIRE(bag.seen[0] == "A_unsubs_B");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_publish_no_subscribers_returns_ok",
          "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    // Publish to a topic no one listens to.  Not an error.
    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "empty.topic",
                                                nullptr, 0) == SAO_STATUS_OK);
    // With payload too.
    const uint8_t payload[] = "hello";
    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "empty.topic",
                                                payload, sizeof(payload))
            == SAO_STATUS_OK);

    sao_engine_event_bus_destroy(bus);
}

// ---------------------------------------------------------------------------
// Recursive publish.
//
// Subscriber A on topic X publishes topic Y from inside its callback.
// Subscriber B on topic Y must receive that event.  Neither publish
// call may deadlock the bus.
// ---------------------------------------------------------------------------
namespace {

struct RecursiveStub {
    sao_engine_event_bus_handle_t bus = nullptr;
    Bag* bag = nullptr;
};

int fireYCallback(const char* /*topic*/, const uint8_t*, size_t,
                  void* user_data) {
    auto* r = static_cast<RecursiveStub*>(user_data);
    r->bag->seen.push_back("A_on_X");
    // Publish Y from inside X's callback.
    (void)sao_engine_event_bus_publish_wave5(r->bus, "Y", nullptr, 0);
    return SAO_ENGINE_EVENT_CONTINUE;
}

}  // namespace

TEST_CASE("event_bus_recursive_publish_handled",
          "[engine][event_bus][wave5]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_wave5(&bus) == SAO_STATUS_OK);

    Bag bag;
    RecursiveStub r{bus, &bag};
    TaggedBag b_on_y{&bag, "B_on_Y", SAO_ENGINE_EVENT_CONTINUE};

    sao_engine_subscription_t as = 0, bs = 0;
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "X", 0,
                                                  &fireYCallback, &r,
                                                  &as) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_wave5(bus, "Y", 0,
                                                  &taggedCallback, &b_on_y,
                                                  &bs) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_wave5(bus, "X", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 2);
    REQUIRE(bag.seen[0] == "A_on_X");
    REQUIRE(bag.seen[1] == "B_on_Y");

    sao_engine_event_bus_destroy(bus);
}
