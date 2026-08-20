// SAO Auto — auto-key injection, hold, and arbitration tests.
//
// Coverage:
//   * send_key + GetAsyncKeyState reflects the injected press after
//     the hold window elapses (proves SendInput actually hits the
//     input stream, not just the queue)
//   * multi-key combo release drains cleanly (no stuck keys reported
//     by the pending-hold ledger)
//   * arbitrate policy honours GAME_ONLY (plugin blocked)
//   * text_utf16 injection completes for a small ASCII+CJK sample

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/auto_key.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// Test-only introspection surface from auto_key.cpp.
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_auto_key_pending_holds(
    void);

namespace {

void wait_for_no_pending_holds(size_t max_ms = 2000) {
    for (size_t i = 0; i < max_ms / 10; ++i) {
        if (sao_ui_auto_key_pending_holds() == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

TEST_CASE("auto_key_send_key_state_reflected",
          "[ui][auto_key][automation]") {
#if defined(_WIN32)
    // Pick a VK that has no real keyboard mapping so the test
    // doesn't interfere with the running shell: VK_F24 (0x87) is the
    // classic "we don't have that key on this keyboard" choice.  It
    // still goes through SendInput and appears in GetAsyncKeyState
    // history until polled.
    constexpr uint32_t kProbeVk = 0x87;

    // Drain any prior async state from previous test runs.
    (void)sao_ui_auto_key_get_key_state(kProbeVk, nullptr);

    // Keep the press alive long enough that a heavily loaded full-suite run
    // cannot schedule the release worker before this process observes down.
    REQUIRE(sao_ui_auto_key_send_key(kProbeVk, 0, /*hold_ms=*/1000)
            == SAO_STATUS_OK);

    // Immediately after emit, the ledger has at least one pending
    // release (the deferred key-up).
    REQUIRE(sao_ui_auto_key_pending_holds() >= 1);

    // GetAsyncKeyState returns 0x8000 (high bit) for a currently held
    // key.  Poll for up to 750 ms — SendInput delivery is nearly
    // synchronous but not guaranteed atomic w.r.t. this thread.
    bool observed_down = false;
    for (int i = 0; i < 75 && !observed_down; ++i) {
        bool state = false;
        REQUIRE(sao_ui_auto_key_get_key_state(kProbeVk, &state)
                == SAO_STATUS_OK);
        if (state) observed_down = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(observed_down);

    // After the hold elapses, the ledger should drain to zero.
    wait_for_no_pending_holds();
    REQUIRE(sao_ui_auto_key_pending_holds() == 0);
#else
    SUCCEED("non-Windows: auto_key send_key is not implemented");
#endif
}

TEST_CASE("auto_key_send_combo_no_stuck",
          "[ui][auto_key][automation]") {
#if defined(_WIN32)
    // Three combo keys — F21 F22 F23.  These have no real mapping on
    // most keyboards so the test doesn't interfere with the shell.
    const uint32_t combo[] = {0x84, 0x85, 0x86};
    REQUIRE(sao_ui_auto_key_send_key_combo(combo, 3, /*hold_ms=*/40)
            == SAO_STATUS_OK);
    // Immediately after: exactly the combo size sits pending.
    REQUIRE(sao_ui_auto_key_pending_holds() >= 3);

    wait_for_no_pending_holds();
    REQUIRE(sao_ui_auto_key_pending_holds() == 0);

    // GetAsyncKeyState on any of the keys should show them released
    // (the high bit is cleared).  Poll briefly — SendInput drain into
    // the input queue can lag the release schedule by one tick.
    for (uint32_t vk : combo) {
        bool released = false;
        for (int i = 0; i < 20 && !released; ++i) {
            bool state = false;
            REQUIRE(sao_ui_auto_key_get_key_state(vk, &state)
                    == SAO_STATUS_OK);
            if (!state) released = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(released);
    }
#else
    SUCCEED("non-Windows: auto_key send_key_combo is not implemented");
#endif
}

TEST_CASE("auto_key_arbitrate_game_only_blocks_plugin",
          "[ui][auto_key][automation]") {
    bool allowed = true;
    REQUIRE(sao_ui_auto_key_arbitrate(SAO_UI_AUTO_KEY_POLICY_GAME_ONLY,
                                       0x74, &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == false);

    allowed = false;
    REQUIRE(sao_ui_auto_key_arbitrate(SAO_UI_AUTO_KEY_POLICY_SHARED,
                                       0x74, &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == true);

    allowed = true;
    REQUIRE(sao_ui_auto_key_arbitrate(SAO_UI_AUTO_KEY_POLICY_BLOCKED,
                                       0x74, &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == false);

    allowed = false;
    REQUIRE(sao_ui_auto_key_arbitrate(SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY,
                                       0x74, &allowed)
            == SAO_STATUS_OK);
    REQUIRE(allowed == true);

    // Invalid VK bounds → allowed=false + error.
    REQUIRE(sao_ui_auto_key_arbitrate(SAO_UI_AUTO_KEY_POLICY_SHARED,
                                       0, &allowed)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(allowed == false);
}

TEST_CASE("auto_key_send_text_utf16",
          "[ui][auto_key][automation]") {
#if defined(_WIN32)
    // ASCII "Hi" + CJK U+4E2D U+6587 (中文) — mixes BMP and non-ASCII
    // UTF-16 code units.  The call succeeds regardless of what window
    // is currently focused; the injected input just goes to whichever
    // window has focus (or nowhere in headless test rigs).
    const wchar_t sample[] = L"Hi中文";
    REQUIRE(sao_ui_auto_key_send_text(
        reinterpret_cast<const uint16_t*>(sample),
        wcslen(sample)) == SAO_STATUS_OK);

    // 0-count with a null-terminated string also works.
    REQUIRE(sao_ui_auto_key_send_text(
        reinterpret_cast<const uint16_t*>(L"x"), 0)
        == SAO_STATUS_OK);

    // Null input is a rejection.
    REQUIRE(sao_ui_auto_key_send_text(nullptr, 0)
            == SAO_STATUS_ERR_INVALID_ARGUMENT);
#else
    SUCCEED("non-Windows: auto_key send_text is not implemented");
#endif
}
