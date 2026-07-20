// SAO Auto — platform/engine/tests/test_event_bus_priority.cpp
//
// Priority API event-bus coverage.
//
// Deterministic scenarios that pin the Python-equivalent semantics:
//   * subscribe / publish delivery
//   * priority ordering (three subscribers)
//   * cancellation stops propagation
//   * compatibility ABI wrappers link and forward
//   * publish_ex options preserve synchronous priority/cancel behavior
//   * unsubscribe-during-fire behaves sanely
//   * publish to an empty topic is OK
//   * recursive publish (a subscriber publishes another event) works

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "sao/engine/event_bus.h"

namespace {

static_assert(
    std::is_same_v<sao_engine_event_wave5_callback_t, sao_engine_event_priority_callback_t>);
static_assert(std::is_same_v<SaoEngineWave5PublishOptions, SaoEnginePriorityPublishOptions>);

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

struct PublishExTrace {
    std::vector<std::string> seen;
    std::thread::id caller_thread;
    bool all_callbacks_on_caller_thread = true;
};

struct PublishExSubscriber {
    PublishExTrace* trace;
    std::string tag;
    int return_code;
};

struct BlockingSubscriber {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic_int calls{0};
};

struct ReentrantDrainSubscriber {
    sao_engine_event_bus_handle_t bus = nullptr;
    sao_engine_subscription_t subscription = 0;
    std::mutex mutex;
    std::condition_variable condition;
    bool first_entered = false;
    bool second_entered = false;
    bool release_first = false;
    std::atomic_int calls{0};
    sao_status_t unsubscribe_status{SAO_STATUS_ERR_UNKNOWN};
};

int taggedCallback(const char* /*topic*/, const uint8_t* /*data*/,
                   size_t /*data_size*/, void* user_data) {
    auto* tb = static_cast<TaggedBag*>(user_data);
    tb->bag->seen.push_back(tb->tag);
    return tb->return_code;
}

int publishExTraceCallback(const char* /*topic*/, const uint8_t* /*data*/, size_t /*data_size*/,
                           void* user_data) {
    auto* subscriber = static_cast<PublishExSubscriber*>(user_data);
    subscriber->trace->all_callbacks_on_caller_thread =
        subscriber->trace->all_callbacks_on_caller_thread &&
        std::this_thread::get_id() == subscriber->trace->caller_thread;
    subscriber->trace->seen.push_back(subscriber->tag);
    return subscriber->return_code;
}

int blockingCallback(const char*, const uint8_t*, size_t, void* user_data) {
    auto* state = static_cast<BlockingSubscriber*>(user_data);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->release; });
    return SAO_ENGINE_EVENT_CONTINUE;
}

int reentrantDrainCallback(const char*, const uint8_t*, size_t, void* user_data) {
    auto* state = static_cast<ReentrantDrainSubscriber*>(user_data);
    const int call = state->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call == 1) {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->first_entered = true;
        state->condition.notify_all();
        state->condition.wait(lock, [state] { return state->release_first; });
    } else if (call == 2) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->second_entered = true;
        }
        state->condition.notify_all();
        state->unsubscribe_status =
            sao_engine_event_bus_unsubscribe(state->bus, state->subscription);
    }
    return SAO_ENGINE_EVENT_CONTINUE;
}

}  // namespace

TEST_CASE("event_bus_subscribe_publish_delivers_to_subscriber",
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag tb{&bag, "sub_a", SAO_ENGINE_EVENT_CONTINUE};
    sao_engine_subscription_t sub = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "topic.x", 0,
                                                  &taggedCallback, &tb,
                                                  &sub) == SAO_STATUS_OK);
    REQUIRE(sub != 0);

    REQUIRE(sao_engine_event_bus_publish_priority(bus, "topic.x",
                                                nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 1);
    REQUIRE(bag.seen[0] == "sub_a");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_legacy_compatibility_symbols_link_and_forward",
          "[engine][event_bus][priority][compatibility]") {
    const auto create_compat = &sao_engine_event_bus_create_wave5;
    const auto subscribe_compat = &sao_engine_event_bus_subscribe_wave5;
    const auto publish_compat = &sao_engine_event_bus_publish_wave5;
    const auto publish_ex_compat = &sao_engine_event_bus_publish_ex_wave5;

    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(create_compat(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag subscriber{&bag, "compat", SAO_ENGINE_EVENT_CONTINUE};
    sao_engine_event_wave5_callback_t callback = &taggedCallback;
    sao_engine_subscription_t subscription = 0;
    REQUIRE(subscribe_compat(bus, "compat.topic", 7, callback, &subscriber, &subscription) ==
            SAO_STATUS_OK);
    REQUIRE(subscription != 0);

    REQUIRE(publish_compat(bus, "compat.topic", nullptr, 0) == SAO_STATUS_OK);
    SaoEngineWave5PublishOptions options{1};
    REQUIRE(publish_ex_compat(bus, "compat.topic", nullptr, 0, &options) == SAO_STATUS_OK);
    CHECK(bag.seen == std::vector<std::string>{"compat", "compat"});

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_priority_ordering", "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag low  {&bag, "low",  SAO_ENGINE_EVENT_CONTINUE};
    TaggedBag mid  {&bag, "mid",  SAO_ENGINE_EVENT_CONTINUE};
    TaggedBag high {&bag, "high", SAO_ENGINE_EVENT_CONTINUE};

    // Register in the "wrong" order so the sort has real work to do.
    sao_engine_subscription_t s1 = 0, s2 = 0, s3 = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 1,
                                                  &taggedCallback, &low,
                                                  &s1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 10,
                                                  &taggedCallback, &high,
                                                  &s2) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 5,
                                                  &taggedCallback, &mid,
                                                  &s3) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_priority(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 3);
    REQUIRE(bag.seen[0] == "high");
    REQUIRE(bag.seen[1] == "mid");
    REQUIRE(bag.seen[2] == "low");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_cancel_stops_propagation",
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    Bag bag;
    TaggedBag first {&bag, "first",  SAO_ENGINE_EVENT_CONTINUE};
    // The second subscriber cancels; the third must never fire.
    TaggedBag second{&bag, "second", SAO_ENGINE_EVENT_CANCELLED};
    TaggedBag third {&bag, "third",  SAO_ENGINE_EVENT_CONTINUE};

    sao_engine_subscription_t s1 = 0, s2 = 0, s3 = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 10,
                                                  &taggedCallback, &first,
                                                  &s1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 5,
                                                  &taggedCallback, &second,
                                                  &s2) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 1,
                                                  &taggedCallback, &third,
                                                  &s3) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_priority(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 2);
    REQUIRE(bag.seen[0] == "first");
    REQUIRE(bag.seen[1] == "second");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_publish_ex_options_remain_synchronous_with_priority_cancel",
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    PublishExTrace trace;
    PublishExSubscriber low{&trace, "low", SAO_ENGINE_EVENT_CONTINUE};
    PublishExSubscriber cancel{&trace, "cancel", SAO_ENGINE_EVENT_CANCELLED};
    PublishExSubscriber high{&trace, "high", SAO_ENGINE_EVENT_CONTINUE};
    sao_engine_subscription_t low_subscription = 0;
    sao_engine_subscription_t cancel_subscription = 0;
    sao_engine_subscription_t high_subscription = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "publish_ex.topic", 1,
                                                    &publishExTraceCallback, &low,
                                                    &low_subscription) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "publish_ex.topic", 5,
                                                    &publishExTraceCallback, &cancel,
                                                    &cancel_subscription) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "publish_ex.topic", 10,
                                                    &publishExTraceCallback, &high,
                                                    &high_subscription) == SAO_STATUS_OK);

    const std::vector<std::string> expected{"high", "cancel"};

    trace.caller_thread = std::this_thread::get_id();
    REQUIRE(sao_engine_event_bus_publish_ex_priority(bus, "publish_ex.topic", nullptr, 0,
                                                     nullptr) == SAO_STATUS_OK);
    CHECK(trace.all_callbacks_on_caller_thread);
    CHECK(trace.seen == expected);

    trace.seen.clear();
    trace.all_callbacks_on_caller_thread = true;
    trace.caller_thread = std::this_thread::get_id();
    const SaoEnginePriorityPublishOptions synchronous_options{0};
    REQUIRE(sao_engine_event_bus_publish_ex_priority(bus, "publish_ex.topic", nullptr, 0,
                                                     &synchronous_options) == SAO_STATUS_OK);
    CHECK(trace.all_callbacks_on_caller_thread);
    CHECK(trace.seen == expected);

    trace.seen.clear();
    trace.all_callbacks_on_caller_thread = true;
    trace.caller_thread = std::this_thread::get_id();
    const SaoEnginePriorityPublishOptions asynchronous_options{1};
    REQUIRE(sao_engine_event_bus_publish_ex_priority(bus, "publish_ex.topic", nullptr, 0,
                                                     &asynchronous_options) == SAO_STATUS_OK);
    CHECK(trace.all_callbacks_on_caller_thread);
    CHECK(trace.seen == expected);

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
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    Bag bag;
    UnsubStub a{};
    a.bus = bus;
    a.bag = &bag;

    TaggedBag b{&bag, "B_fired", SAO_ENGINE_EVENT_CONTINUE};

    // Subscribe B first so we know its token, then A which will remove
    // B mid-fire.  A has higher priority so it runs first.
    sao_engine_subscription_t bs = 0, as = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 1,
                                                  &taggedCallback, &b,
                                                  &bs) == SAO_STATUS_OK);
    a.target = bs;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "t", 10,
                                                  &unsubberCallback, &a,
                                                  &as) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_priority(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);

    REQUIRE(a.ran);
    // B may or may not have fired depending on snapshot-vs-live-check;
    // either behaviour is acceptable as long as bag.seen[0] is A and
    // there is no crash.
    REQUIRE(bag.seen.size() >= 1);
    REQUIRE(bag.seen[0] == "A_unsubs_B");
    // A second publish must see B gone.
    bag.seen.clear();
    REQUIRE(sao_engine_event_bus_publish_priority(bus, "t", nullptr, 0)
            == SAO_STATUS_OK);
    // Only "A_unsubs_B" this time — B was pruned.
    REQUIRE(bag.seen.size() == 1);
    REQUIRE(bag.seen[0] == "A_unsubs_B");

    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event bus unsubscribe drains callbacks already dispatched on another thread",
          "[engine][event_bus][priority][lifetime][concurrency]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);
    BlockingSubscriber state;
    sao_engine_subscription_t subscription = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "drain", 0, blockingCallback, &state,
                                                    &subscription) == SAO_STATUS_OK);

    auto publish = std::async(std::launch::async, [&] {
        return sao_engine_event_bus_publish_priority(bus, "drain", nullptr, 0);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.entered; }));
    }
    auto unsubscribe = std::async(std::launch::async, [&] {
        return sao_engine_event_bus_unsubscribe(bus, subscription);
    });
    CHECK(unsubscribe.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.condition.notify_all();
    REQUIRE(publish.get() == SAO_STATUS_OK);
    REQUIRE(unsubscribe.get() == SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(sao_engine_event_bus_publish_priority(bus, "drain", nullptr, 0) == SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 1);
    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event bus self unsubscribe drains only callbacks owned by other threads",
          "[engine][event_bus][priority][lifetime][concurrency][reentry]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);
    ReentrantDrainSubscriber state;
    state.bus = bus;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "self-drain", 0,
                                                    reentrantDrainCallback, &state,
                                                    &state.subscription) == SAO_STATUS_OK);

    auto first_publish = std::async(std::launch::async, [&] {
        return sao_engine_event_bus_publish_priority(bus, "self-drain", nullptr, 0);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.first_entered; }));
    }

    auto second_publish = std::async(std::launch::async, [&] {
        return sao_engine_event_bus_publish_priority(bus, "self-drain", nullptr, 0);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&state] { return state.second_entered; }));
    }
    CHECK(second_publish.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release_first = true;
    }
    state.condition.notify_all();
    REQUIRE(first_publish.get() == SAO_STATUS_OK);
    REQUIRE(second_publish.get() == SAO_STATUS_OK);
    CHECK(state.unsubscribe_status == SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 2);
    REQUIRE(sao_engine_event_bus_publish_priority(bus, "self-drain", nullptr, 0) ==
            SAO_STATUS_OK);
    CHECK(state.calls.load(std::memory_order_relaxed) == 2);
    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("event_bus_publish_no_subscribers_returns_ok",
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    // Publish to a topic no one listens to.  Not an error.
    REQUIRE(sao_engine_event_bus_publish_priority(bus, "empty.topic",
                                                nullptr, 0) == SAO_STATUS_OK);
    // With payload too.
    const uint8_t payload[] = "hello";
    REQUIRE(sao_engine_event_bus_publish_priority(bus, "empty.topic",
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
    (void)sao_engine_event_bus_publish_priority(r->bus, "Y", nullptr, 0);
    return SAO_ENGINE_EVENT_CONTINUE;
}

}  // namespace

TEST_CASE("event_bus_recursive_publish_handled",
          "[engine][event_bus][priority]") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&bus) == SAO_STATUS_OK);

    Bag bag;
    RecursiveStub r{bus, &bag};
    TaggedBag b_on_y{&bag, "B_on_Y", SAO_ENGINE_EVENT_CONTINUE};

    sao_engine_subscription_t as = 0, bs = 0;
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "X", 0,
                                                  &fireYCallback, &r,
                                                  &as) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe_priority(bus, "Y", 0,
                                                  &taggedCallback, &b_on_y,
                                                  &bs) == SAO_STATUS_OK);

    REQUIRE(sao_engine_event_bus_publish_priority(bus, "X", nullptr, 0)
            == SAO_STATUS_OK);
    REQUIRE(bag.seen.size() == 2);
    REQUIRE(bag.seen[0] == "A_on_X");
    REQUIRE(bag.seen[1] == "B_on_Y");

    sao_engine_event_bus_destroy(bus);
}
