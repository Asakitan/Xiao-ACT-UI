#include <sao_core/sao_core.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#define sao_core_set_structured_log_callback sao_legacy_core_set_structured_log_callback
#define sao_core_set_log_callback sao_legacy_core_set_log_callback
#define sao_core_scan_find_pattern sao_legacy_core_scan_find_pattern
#define sao_core_pixels_sample_bgra sao_legacy_core_pixels_sample_bgra
#define SaoCoreBgraPixel SaoLegacyCoreBgraPixel
#define SAO_CORE_LOG_LEVEL_ERROR SAO_LEGACY_CORE_LOG_LEVEL_ERROR

namespace {

using namespace std::chrono_literals;

constexpr int32_t kTriggerFailureStatus = SAO_ERR_NOT_FOUND;

int32_t trigger_failure();
std::string g_legacy_message;

void SAO_LEGACY_CORE_CALL capture_legacy_log(int32_t, const char* message) {
    g_legacy_message = message;
}

struct CallbackReset final {
    ~CallbackReset() {
        (void)sao_core_set_structured_log_callback(nullptr, nullptr);
        sao_core_set_log_callback(nullptr);
    }
};

struct LogRecord final {
    uint32_t calls{};
    int32_t level{};
    std::string component;
    int32_t status{};
    std::string message;
};

void SAO_LEGACY_CORE_CALL capture_log(int32_t level, const char* component, int32_t status,
                                      const char* message, void* user_data) {
    auto* record = static_cast<LogRecord*>(user_data);
    ++record->calls;
    record->level = level;
    record->component = component;
    record->status = status;
    record->message = message;
}

struct BlockingCallback final {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool release{false};
    std::atomic_uint32_t calls{0};
};

void SAO_LEGACY_CORE_CALL blocking_log(int32_t, const char*, int32_t, const char*,
                                       void* user_data) {
    auto* state = static_cast<BlockingCallback*>(user_data);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->release; });
}

void SAO_LEGACY_CORE_CALL counting_log(int32_t, const char*, int32_t, const char*,
                                       void* user_data) {
    static_cast<std::atomic_uint32_t*>(user_data)->fetch_add(1, std::memory_order_relaxed);
}

void SAO_LEGACY_CORE_CALL reentrant_clear_log(int32_t, const char*, int32_t, const char*,
                                              void* user_data) {
    static_cast<std::atomic_uint32_t*>(user_data)->fetch_add(1, std::memory_order_relaxed);
    (void)trigger_failure();
    (void)sao_core_set_structured_log_callback(nullptr, nullptr);
}

void SAO_LEGACY_CORE_CALL throwing_log(int32_t, const char*, int32_t, const char*, void*) {
    throw std::runtime_error("callback failure");
}

void SAO_LEGACY_CORE_CALL seh_log(int32_t, const char*, int32_t, const char*, void*) {
    RaiseException(0xE0421001U, 0, 0, nullptr);
}

int32_t trigger_failure() {
    const uint8_t pattern = 0x42;
    const uint8_t mask = 0xFF;
    std::size_t offset = 0;
    return sao_core_scan_find_pattern(nullptr, 0, &pattern, &mask, 1, &offset);
}

} // namespace

TEST_CASE("legacy core emits structured operation failures", "[logging]") {
    CallbackReset reset;
    LogRecord record;

    REQUIRE(sao_core_set_structured_log_callback(&capture_log, &record) == SAO_OK);
    REQUIRE(trigger_failure() == kTriggerFailureStatus);
    CHECK(record.level == SAO_CORE_LOG_LEVEL_ERROR);
    CHECK(record.component == "core.scan");
    CHECK(record.status == kTriggerFailureStatus);
    CHECK(record.message.find("scan_find_pattern") != std::string::npos);
    CHECK(record.calls == 1);
}

TEST_CASE("legacy core records nested operation failure once", "[logging]") {
    CallbackReset reset;
    LogRecord record;
    REQUIRE(sao_core_set_structured_log_callback(&capture_log, &record) == SAO_OK);

    const uint8_t bgra[4]{};
    SaoCoreBgraPixel pixel{};
    REQUIRE(sao_core_pixels_sample_bgra(bgra, sizeof(bgra), 1, 1, 3, 0, 0, &pixel) ==
            SAO_ERR_INVALID_ARGUMENT);
    CHECK(record.calls == 1);
    CHECK(record.component == "core.pixels");
    CHECK(record.message.find("pixels_sample_bgra") != std::string::npos);
}

TEST_CASE("legacy core emits through the original callback ABI", "[logging]") {
    CallbackReset reset;
    g_legacy_message.clear();
    sao_core_set_log_callback(&capture_legacy_log);

    REQUIRE(trigger_failure() == kTriggerFailureStatus);
    CHECK(g_legacy_message.find("component=core.scan") != std::string::npos);
    CHECK(g_legacy_message.find("status=-7") != std::string::npos);
    CHECK(g_legacy_message.find("scan_find_pattern") != std::string::npos);
}

TEST_CASE("legacy core replacement retires old userdata", "[logging][race]") {
    CallbackReset reset;
    BlockingCallback old_callback;
    std::atomic_uint32_t replacement_calls{0};
    REQUIRE(sao_core_set_structured_log_callback(&blocking_log, &old_callback) == SAO_OK);

    std::atomic_int32_t emitter_status{SAO_ERR_UNKNOWN};
    std::thread emitter(
        [&emitter_status] { emitter_status.store(trigger_failure(), std::memory_order_relaxed); });
    {
        std::unique_lock lock(old_callback.mutex);
        REQUIRE(old_callback.condition.wait_for(lock, 2s,
                                                [&old_callback] { return old_callback.entered; }));
    }

    auto replacement = std::async(std::launch::async, [&replacement_calls] {
        return sao_core_set_structured_log_callback(&counting_log, &replacement_calls);
    });
    CHECK(replacement.wait_for(50ms) == std::future_status::timeout);

    {
        std::lock_guard lock(old_callback.mutex);
        old_callback.release = true;
    }
    old_callback.condition.notify_all();
    emitter.join();
    REQUIRE(replacement.get() == SAO_OK);

    CHECK(emitter_status.load(std::memory_order_relaxed) == kTriggerFailureStatus);
    REQUIRE(trigger_failure() == kTriggerFailureStatus);
    CHECK(old_callback.calls.load(std::memory_order_relaxed) == 1);
    CHECK(replacement_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("legacy core callback may clear itself", "[logging][reentry]") {
    CallbackReset reset;
    std::atomic_uint32_t calls{0};
    REQUIRE(sao_core_set_structured_log_callback(&reentrant_clear_log, &calls) == SAO_OK);

    REQUIRE(trigger_failure() == kTriggerFailureStatus);
    REQUIRE(trigger_failure() == kTriggerFailureStatus);
    CHECK(calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("legacy core isolates callback exceptions", "[logging]") {
    CallbackReset reset;
    REQUIRE(sao_core_set_structured_log_callback(&throwing_log, nullptr) == SAO_OK);
    int32_t status = SAO_ERR_UNKNOWN;
    CHECK_NOTHROW(status = trigger_failure());
    CHECK(status == kTriggerFailureStatus);

    REQUIRE(sao_core_set_structured_log_callback(&seh_log, nullptr) == SAO_OK);
    status = SAO_ERR_UNKNOWN;
    CHECK_NOTHROW(status = trigger_failure());
    CHECK(status == kTriggerFailureStatus);
}
