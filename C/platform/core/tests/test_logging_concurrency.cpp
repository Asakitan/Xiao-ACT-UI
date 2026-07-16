#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/core/logging.h"

namespace {

struct CallbackState {
    int marker;
    std::atomic<int>* calls;
    std::atomic<int>* mismatches;
};

void SAO_CORE_CALL callback_one(int32_t, const char*, const char*, void* user_data) {
    auto* state = static_cast<CallbackState*>(user_data);
    state->calls->fetch_add(1, std::memory_order_relaxed);
    if (state->marker != 1) {
        state->mismatches->fetch_add(1, std::memory_order_relaxed);
    }
}

void SAO_CORE_CALL callback_two(int32_t, const char*, const char*, void* user_data) {
    auto* state = static_cast<CallbackState*>(user_data);
    state->calls->fetch_add(1, std::memory_order_relaxed);
    if (state->marker != 2) {
        state->mismatches->fetch_add(1, std::memory_order_relaxed);
    }
}

void SAO_CORE_CALL reentrant_callback(int32_t, const char*, const char*,
                                      void* user_data) {
    auto* calls = static_cast<int*>(user_data);
    ++*calls;
    (void)sao_core_set_log_level(SAO_LOG_WARN);
    (void)sao_core_set_log_callback(nullptr, nullptr);
}

struct LoggingReset {
    ~LoggingReset() {
        (void)sao_core_set_log_callback(nullptr, nullptr);
        (void)sao_core_set_log_level(SAO_LOG_INFO);
    }
};

struct BlockingCallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
};

void SAO_CORE_CALL blocking_callback(int32_t, const char*, const char*,
                                     void* user_data) {
    auto* state = static_cast<BlockingCallbackState*>(user_data);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->released; });
}

}  // namespace

TEST_CASE("core logging callback can reconfigure logging",
          "[core][logging][concurrency]") {
    LoggingReset reset;
    int calls = 0;
    REQUIRE(sao_core_set_log_level(SAO_LOG_TRACE) == SAO_STATUS_OK);
    REQUIRE(sao_core_set_log_callback(&reentrant_callback, &calls) ==
            SAO_STATUS_OK);
    REQUIRE(sao_core_log(SAO_LOG_INFO, "core.test", "first") ==
            SAO_STATUS_OK);
    REQUIRE(calls == 1);
    REQUIRE(sao_core_log(SAO_LOG_FATAL, "core.test", "second") ==
            SAO_STATUS_OK);
    REQUIRE(calls == 1);
}

TEST_CASE("core logging snapshots callback and user data atomically",
          "[core][logging][concurrency]") {
    LoggingReset reset;
    std::atomic<int> calls{0};
    std::atomic<int> mismatches{0};
    CallbackState first{1, &calls, &mismatches};
    CallbackState second{2, &calls, &mismatches};
    REQUIRE(sao_core_set_log_level(SAO_LOG_TRACE) == SAO_STATUS_OK);

    std::thread writer([&] {
        for (int index = 0; index < 10000; ++index) {
            if ((index & 1) == 0) {
                (void)sao_core_set_log_callback(&callback_one, &first);
            } else {
                (void)sao_core_set_log_callback(&callback_two, &second);
            }
        }
    });
    std::vector<std::thread> readers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        readers.emplace_back([] {
            for (int index = 0; index < 10000; ++index) {
                (void)sao_core_logf(SAO_LOG_INFO, "core.test", "message-%d",
                                    index);
            }
        });
    }
    writer.join();
    for (auto& reader : readers) reader.join();

    REQUIRE(calls.load(std::memory_order_relaxed) > 0);
    REQUIRE(mismatches.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("core logging unregister waits for an in-flight callback",
          "[core][logging][concurrency]") {
    using namespace std::chrono_literals;

    LoggingReset reset;
    BlockingCallbackState state;
    REQUIRE(sao_core_set_log_callback(&blocking_callback, &state) ==
            SAO_STATUS_OK);

    auto logger = std::async(std::launch::async, [] {
        return sao_core_log(SAO_LOG_INFO, "core.test", "blocking");
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait(lock, [&state] { return state.entered; });
    }

    auto unregister = std::async(std::launch::async, [] {
        return sao_core_set_log_callback(nullptr, nullptr);
    });
    REQUIRE(unregister.wait_for(20ms) == std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.released = true;
        state.condition.notify_all();
    }
    REQUIRE(logger.get() == SAO_STATUS_OK);
    REQUIRE(unregister.get() == SAO_STATUS_OK);
}