// SAO Auto — launcher/tests/test_launcher_smoke.cpp
//
// Smoke test: verify AppState default construction, exit code enum values
// match the README table, and the launcher exports the right symbols.

#include <catch2/catch_test_macros.hpp>

#include "sao/launcher/app.h"
#include "sao/launcher/init_pipeline.h"

using namespace sao::launcher;

namespace {

constexpr UINT_PTR kForeignTimerId = ~UINT_PTR{0};
constexpr int kTickExitCode = 37;
constexpr int kUnexpectedTimerExitCode = 38;
constexpr DWORD kTimerObservationDelayMs = 50;

struct MessageLoopRecorder {
    sao_status_t tick_status = SAO_STATUS_OK;
    sao_status_t message_status = SAO_STATUS_OK;
    int tick_count = 0;
    uint32_t elapsed_ms = 0;
    int message_count = 0;
    uint32_t last_message = 0;
    uintptr_t last_w_param = 0;
    UINT_PTR unexpected_timer_id = 0;

    static sao_status_t tick(sao_platform_ctx*, uint32_t elapsed,
                             void* user_data) {
        auto* self = static_cast<MessageLoopRecorder*>(user_data);
        ++self->tick_count;
        self->elapsed_ms = elapsed;
        if (self->tick_status == SAO_STATUS_OK) {
            PostQuitMessage(kTickExitCode);
        }
        return self->tick_status;
    }

    static sao_status_t handleMessage(sao_platform_ctx*, uint32_t message,
                                      uintptr_t w_param, intptr_t,
                                      int32_t* out_handled,
                                      void* user_data) {
        auto* self = static_cast<MessageLoopRecorder*>(user_data);
        ++self->message_count;
        self->last_message = message;
        self->last_w_param = w_param;
        if (out_handled) {
            *out_handled = 1;
        }
        if (message == WM_TIMER && w_param != kForeignTimerId) {
            self->unexpected_timer_id = w_param;
            PostQuitMessage(kUnexpectedTimerExitCode);
        }
        return self->message_status;
    }
};

struct MessageLoopHookGuard {
    explicit MessageLoopHookGuard(MessageLoopRecorder& recorder) {
        hooks.ui_tick = &MessageLoopRecorder::tick;
        hooks.ui_handle_message = &MessageLoopRecorder::handleMessage;
        hooks.user_data = &recorder;
        sao_launcher_set_composition_test_hooks(&hooks);
    }

    ~MessageLoopHookGuard() {
        sao_launcher_set_composition_test_hooks(nullptr);
    }

    sao_launcher_composition_test_hooks_t hooks{};
};

void drainTimerMessages() {
    MSG message{};
    while (PeekMessageW(
            &message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
        (void)KillTimer(nullptr, message.wParam);
    }
}

bool timerMessageAppearsAfterExit() {
    Sleep(kTimerObservationDelayMs);
    MSG message{};
    bool found = false;
    while (PeekMessageW(
            &message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
        found = true;
        (void)KillTimer(nullptr, message.wParam);
    }
    return found;
}

} // namespace

TEST_CASE("Exit code enum matches README contract", "[launcher][smoke]") {
    REQUIRE(SAO_EXIT_OK                 == 0);
    REQUIRE(SAO_EXIT_ALREADY_RUNNING    == 1);
    REQUIRE(SAO_EXIT_LICENSE_INVALID    == 2);
    REQUIRE(SAO_EXIT_SHELL_TAMPERED     == 3);
    REQUIRE(SAO_EXIT_PLATFORM_INIT_FAIL == 4);
    REQUIRE(SAO_EXIT_PLUGIN_LOAD_FAIL   == 5);
    REQUIRE(SAO_EXIT_UI_ONLINE_FAIL     == 6);
    REQUIRE(SAO_EXIT_CRASH              == 7);
    REQUIRE(SAO_EXIT_BAD_ARGS           == 8);
    REQUIRE(SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL == 9);
}

TEST_CASE("launcher message loop ticks only its generated timer",
          "[launcher][ui][timer]") {
    drainTimerMessages();
    MessageLoopRecorder recorder;
    MessageLoopHookGuard hook_guard(recorder);

    REQUIRE(PostThreadMessageW(
        GetCurrentThreadId(), WM_TIMER, kForeignTimerId, 0));
    const int result = App::instance().runMessageLoop();
    const bool leaked_timer = timerMessageAppearsAfterExit();

    CHECK(result == kTickExitCode);
    CHECK(recorder.tick_count == 1);
    CHECK(recorder.elapsed_ms == 16);
    CHECK(recorder.message_count == 1);
    CHECK(recorder.last_message == WM_TIMER);
    CHECK(recorder.last_w_param == kForeignTimerId);
    CHECK(recorder.unexpected_timer_id == 0);
    CHECK_FALSE(leaked_timer);
}

TEST_CASE("launcher message loop kills its timer when tick fails",
          "[launcher][ui][timer][failure]") {
    drainTimerMessages();
    MessageLoopRecorder recorder;
    recorder.tick_status = SAO_STATUS_INTERNAL;
    MessageLoopHookGuard hook_guard(recorder);

    const int result = App::instance().runMessageLoop();
    const bool leaked_timer = timerMessageAppearsAfterExit();

    CHECK(result == SAO_EXIT_UI_ONLINE_FAIL);
    CHECK(recorder.tick_count == 1);
    CHECK(recorder.elapsed_ms == 16);
    CHECK(recorder.message_count == 0);
    CHECK(recorder.unexpected_timer_id == 0);
    CHECK_FALSE(leaked_timer);
}

TEST_CASE("launcher message loop kills its timer when message handling fails",
          "[launcher][ui][timer][failure]") {
    drainTimerMessages();
    MessageLoopRecorder recorder;
    recorder.message_status = SAO_STATUS_INTERNAL;
    MessageLoopHookGuard hook_guard(recorder);
    constexpr UINT kTestMessage = WM_APP + 7;

    REQUIRE(PostThreadMessageW(
        GetCurrentThreadId(), kTestMessage, 123, 456));
    const int result = App::instance().runMessageLoop();
    const bool leaked_timer = timerMessageAppearsAfterExit();

    CHECK(result == SAO_EXIT_UI_ONLINE_FAIL);
    CHECK(recorder.tick_count == 0);
    CHECK(recorder.message_count == 1);
    CHECK(recorder.last_message == kTestMessage);
    CHECK(recorder.last_w_param == 123);
    CHECK_FALSE(leaked_timer);
}
